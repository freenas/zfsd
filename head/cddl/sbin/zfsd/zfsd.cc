/*-
 * Copyright (c) 2011 Spectra Logic Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 *
 * Authors: Justin T. Gibbs     (Spectra Logic Corporation)
 */

/**
 * \file zfsd.cc
 *
 * The ZFS daemon consumes kernel devctl(4) event data via devd(8)'s
 * unix domain socket in order to react to system changes that impact
 * the function of ZFS storage pools.  The goal of this daemon is to
 * provide similar functionality to the Solaris ZFS Diagnostic Engine
 * (zfs-diagnosis), the Solaris ZFS fault handler (zfs-retire), and
 * the Solaris ZFS vdev insertion agent (zfs-mod sysevent handler).
 */

#include <sys/cdefs.h>
#include <sys/disk.h>
#include <sys/filio.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <libutil.h>
#include <sstream>
#include <string>
#include <syslog.h>

#include <sys/fs/zfs.h>
#include <libzfs.h>
#include <libgeom.h>

#include "callout.h"
#include "vdev.h"
#include "zfsd.h"
#include "zfsd_exception.h"
#include "zpool_list.h"

__FBSDID("$FreeBSD$");

/*============================ Namespace Control =============================*/
using std::string;
using std::stringstream;
using std::cerr;
using std::cout;
using std::endl;

/*================================ Global Data ===============================*/
const char       g_devdSock[] = "/var/run/devd.pipe";
int              g_debug = 0;
libzfs_handle_t *g_zfsHandle;

/*-------------------------------- FDReader    -------------------------------*/
//- FDReader Public Methods ----------------------------------------------------
size_t
FDReader::in_avail() const
{
	int bytes;
	if (ioctl(m_fd, FIONREAD, &bytes)) {
		syslog(LOG_ERR, "ioctl FIONREAD: %s", strerror(errno));
		return (0);
	}
	return (bytes);
}


/*-------------------------------- IstreamReader   ---------------------------*/
//- IstreamReader Public Methods ----------------------------------------------
ssize_t
IstreamReader::read(char* buf, size_t count)
{
	m_stream->read(buf, count);
	if (m_stream->fail())
		return (-1);
	return (m_stream->gcount());
}


/*-------------------------------- EventBuffer -------------------------------*/
//- EventBuffer Static Data ----------------------------------------------------
/**
 * NOTIFY, NOMATCH, ATTACH, DETACH.  See DevCtlEvent::Type.
 */
const char EventBuffer::s_eventStartTokens[] = "!?+-";

/**
 * Events are terminated by a newline.
 */
const char EventBuffer::s_eventEndTokens[] = "\n";

/**
 * Key=Value pairs are terminated by whitespace.
 */
const char EventBuffer::s_keyPairSepTokens[] = " \t\n";

//- EventBuffer Public Methods -------------------------------------------------
EventBuffer::EventBuffer(Reader& reader)
 : m_reader(reader),
   m_validLen(0),
   m_parsedLen(0),
   m_nextEventOffset(0),
   m_synchronized(true)
{
}

bool
EventBuffer::ExtractEvent(string &eventString)
{
	stringstream tsField;
	timeval now;

	gettimeofday(&now, NULL);
	tsField << " timestamp=" << now.tv_sec;

	while (UnParsed() > 0 || Fill()) {

		/*
		 * If the valid data in the buffer isn't enough to hold
		 * a full event, try reading more.
		 */
		if (NextEventMaxLen() < MIN_EVENT_SIZE) {
			m_parsedLen += UnParsed();
			continue;
		}

		char  *nextEvent(m_buf + m_nextEventOffset);
		bool   truncated(true);
		size_t eventLen(strcspn(nextEvent, s_eventEndTokens));

		if (!m_synchronized) {
			/* Discard data until an end token is read. */
			if (nextEvent[eventLen] != '\0')
				m_synchronized = true;
			m_nextEventOffset += eventLen;
			m_parsedLen = m_nextEventOffset;
			continue;
		} else if (nextEvent[eventLen] == '\0') {

			m_parsedLen += eventLen;
			if (m_parsedLen < MAX_EVENT_SIZE) {
				/*
				 * Ran out of buffer before hitting
				 * a full event. Fill() and try again.
				 */
				continue;
			}
			syslog(LOG_WARNING, "Overran event buffer\n\tm_nextEventOffset"
			       "=%d\n\tm_parsedLen=%d\n\tm_validLen=%d",
			       m_nextEventOffset, m_parsedLen, m_validLen);
		} else {
			/*
			 * Include the normal terminator in the extracted
			 * event data.
			 */
			eventLen += 1;
			truncated = false;
		}

		m_nextEventOffset += eventLen;
		m_parsedLen = m_nextEventOffset;
		eventString.assign(nextEvent, eventLen);

		if (truncated) {
			size_t fieldEnd;

			/* Break cleanly at the end of a key<=>value pair. */
			fieldEnd = eventString.find_last_of(s_keyPairSepTokens);
			if (fieldEnd != string::npos)
				eventString.erase(fieldEnd);
			eventString += '\n';

			m_synchronized = false;
			syslog(LOG_WARNING,
			       "Truncated %zd characters from event.",
			       eventLen - fieldEnd);
		}

		/*
		 * Add a timestamp as the final field of the event if it is
		 * not already present.
		 */
		if (eventString.find("timestamp=") == string::npos) {
			size_t eventEnd(eventString.find_last_not_of('\n') + 1);

			eventString.insert(eventEnd, tsField.str());
		}

		return (true);
	}
	return (false);
}

//- EventBuffer Private Methods ------------------------------------------------
bool
EventBuffer::Fill()
{
	size_t avail;
	ssize_t consumed(0);

	/* Compact the buffer. */
	if (m_nextEventOffset != 0) {
		memmove(m_buf, m_buf + m_nextEventOffset,
			m_validLen - m_nextEventOffset);
		m_validLen -= m_nextEventOffset;
		m_parsedLen -= m_nextEventOffset;
		m_nextEventOffset = 0;
	}

	/* Fill any empty space. */
	avail = m_reader.in_avail();
	if (avail) {
		size_t want;

		want = std::min(avail, MAX_READ_SIZE - m_validLen);
		consumed = m_reader.read(m_buf + m_validLen, want);
		if (consumed == -1) {
			if (errno == EINTR)
				return (false);
			else
				err(1, "EventBuffer::Fill(): Read failed");
		}
	}

	m_validLen += consumed;
	/* Guarantee our buffer is always NUL terminated. */
	m_buf[m_validLen] = '\0';

	return (consumed > 0);
}

/*--------------------------------- ZfsDaemon --------------------------------*/
//- ZfsDaemon Static Private Data ----------------------------------------------
bool             ZfsDaemon::s_logCaseFiles;
bool             ZfsDaemon::s_terminateEventLoop;
char             ZfsDaemon::s_pidFilePath[] = "/var/run/zfsd.pid";
pidfh           *ZfsDaemon::s_pidFH;
FDReader*        ZfsDaemon::s_reader;
int              ZfsDaemon::s_devdSockFD = -1;
int              ZfsDaemon::s_signalPipeFD[2];
bool		 ZfsDaemon::s_systemRescanRequested(false);
bool		 ZfsDaemon::s_consumingEvents(false);
DevCtlEventList	 ZfsDaemon::s_unconsumedEvents;

//- ZfsDaemon Static Public Methods --------------------------------------------
void
ZfsDaemon::WakeEventLoop()
{
	write(s_signalPipeFD[1], "+", 1);
}

void
ZfsDaemon::RequestSystemRescan()
{
	s_systemRescanRequested = true;
	ZfsDaemon::WakeEventLoop();
}

void
ZfsDaemon::Run()
{
	Init();

	while (s_terminateEventLoop == false) {

		try {
			DisconnectFromDevd();

			if (ConnectToDevd() == false) {
				sleep(30);
				continue;
			}

			DetectMissedEvents();

			EventLoop();

		} catch (const ZfsdException &exp) {
			exp.Log();
		}
	}

	DisconnectFromDevd();

	Fini();
}

//- ZfsDaemon Static Private Methods -------------------------------------------
void
ZfsDaemon::Init()
{
	if (pipe(s_signalPipeFD) != 0)
		errx(1, "Unable to allocate signal pipe. Exiting");

	if (fcntl(s_signalPipeFD[0], F_SETFL, O_NONBLOCK) == -1)
		errx(1, "Unable to set pipe as non-blocking. Exiting");

	if (fcntl(s_signalPipeFD[1], F_SETFL, O_NONBLOCK) == -1)
		errx(1, "Unable to set pipe as non-blocking. Exiting");

	signal(SIGHUP,  ZfsDaemon::RescanSignalHandler);
	signal(SIGINFO, ZfsDaemon::InfoSignalHandler);
	signal(SIGINT,  ZfsDaemon::QuitSignalHandler);
	signal(SIGTERM, ZfsDaemon::QuitSignalHandler);
	signal(SIGUSR1, ZfsDaemon::RescanSignalHandler);

	g_zfsHandle = libzfs_init();
	if (g_zfsHandle == NULL)
		errx(1, "Unable to initialize ZFS library. Exiting");

	Callout::Init();
	DevCtlEvent::Init();
	InitializeSyslog();
	OpenPIDFile();

	if (g_debug == 0)
		daemon(0, 0);

	UpdatePIDFile();
}

void
ZfsDaemon::Fini()
{
	PurgeCaseFiles();
	ClosePIDFile();
}

void
ZfsDaemon::InfoSignalHandler(int)
{
	s_logCaseFiles = true;
	ZfsDaemon::WakeEventLoop();
}

void
ZfsDaemon::RescanSignalHandler(int)
{
	RequestSystemRescan();
}

void
ZfsDaemon::QuitSignalHandler(int)
{
	s_terminateEventLoop = true;
	ZfsDaemon::WakeEventLoop();
}

void
ZfsDaemon::OpenPIDFile()
{
	pid_t otherPID;

	s_pidFH = pidfile_open(s_pidFilePath, 0600, &otherPID);
	if (s_pidFH == NULL) {
		if (errno == EEXIST)
			errx(1, "already running as PID %d. Exiting", otherPID);
		warn("cannot open PID file");
	}
}

void
ZfsDaemon::UpdatePIDFile()
{
	if (s_pidFH != NULL)
		pidfile_write(s_pidFH);
}

void
ZfsDaemon::ClosePIDFile()
{
	if (s_pidFH != NULL)
		pidfile_close(s_pidFH);
}

void
ZfsDaemon::InitializeSyslog()
{
	openlog("zfsd", LOG_NDELAY, LOG_DAEMON);
}

bool
ZfsDaemon::ConnectToDevd()
{
	struct sockaddr_un devdAddr;
	int		   sLen;
	int		   result;

	syslog(LOG_INFO, "Connecting to devd");

	memset(&devdAddr, 0, sizeof(devdAddr));
	devdAddr.sun_family= AF_UNIX;
	strlcpy(devdAddr.sun_path, g_devdSock, sizeof(devdAddr.sun_path));
	sLen = SUN_LEN(&devdAddr);

	s_devdSockFD = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s_devdSockFD == -1)
		err(1, "Unable to create socket");
	result = connect(s_devdSockFD,
			 reinterpret_cast<sockaddr *>(&devdAddr),
			 sLen);
	if (result == -1) {
		syslog(LOG_INFO, "Unable to connect to devd");
		return (false);
	}


	/* Connect the stream to the file descriptor */
	s_reader = new FDReader(s_devdSockFD);
	syslog(LOG_INFO, "Connection to devd successful");
	return (true);
}

void
ZfsDaemon::DisconnectFromDevd()
{
	if (s_devdSockFD != -1)
		syslog(LOG_INFO, "Disconnecting from devd.");

	delete s_reader;
	s_reader = NULL;
	close(s_devdSockFD);
	s_devdSockFD = -1;
}

void
ZfsDaemon::ReplayUnconsumedEvents()
{
	DevCtlEventList::iterator event(s_unconsumedEvents.begin());
	bool replayed_any = (event != s_unconsumedEvents.end());

	s_consumingEvents = true;
	if (replayed_any)
		syslog(LOG_INFO, "Started replaying unconsumed events");
	while (event != s_unconsumedEvents.end()) {
		(*event)->Process();
		delete *event;
		s_unconsumedEvents.erase(event++);
	}
	if (replayed_any)
		syslog(LOG_INFO, "Finished replaying unconsumed events");
	s_consumingEvents = false;
}

bool
ZfsDaemon::SaveEvent(const DevCtlEvent &event)
{
	if (s_consumingEvents)
		return (false);
	s_unconsumedEvents.push_back(event.DeepCopy());
	return (true);
}

/* Capture and process buffered events. */
void
ZfsDaemon::ProcessEvents(EventBuffer &eventBuffer)
{
	string evString;
	while (eventBuffer.ExtractEvent(evString)) {
		DevCtlEvent *event(DevCtlEvent::CreateEvent(evString));
		if (event != NULL) {
			event->Process();
			delete event;
		}
	}
}

void
ZfsDaemon::FlushEvents()
{
	char discardBuf[256];

	while (s_reader->in_avail())
		s_reader->read(discardBuf, sizeof(discardBuf));
}

bool
ZfsDaemon::EventsPending()
{
	struct pollfd fds[1];
	int	      result;

	do {
		fds->fd      = s_devdSockFD;
		fds->events  = POLLIN;
		fds->revents = 0;
		result = poll(fds, NUM_ELEMENTS(fds), /*timeout*/0);
	} while (result == -1 && errno == EINTR);

	if (result == -1)
		err(1, "Polling for devd events failed");

	if ((fds->revents & POLLERR) != 0)
		throw ZfsdException("ZfsdDaemon:EventsPending(): "
				    "POLLERR detected on devd socket.");

	if ((fds->revents & POLLHUP) != 0)
		throw ZfsdException("ZfsDaemon::EventsPending(): "
				    "POLLHUP detected on devd socket.");

	return ((fds->revents & POLLIN) != 0);
}

void
ZfsDaemon::PurgeCaseFiles()
{
	CaseFile::PurgeAll();
}

bool
ZfsDaemon::VdevAddCaseFile(Vdev &vdev, void *cbArg)
{

	if (vdev.State() != VDEV_STATE_HEALTHY)
		CaseFile::Create(vdev);

	return (/*break early*/false);
}

void
ZfsDaemon::BuildCaseFiles()
{
	/* Add CaseFiles for vdevs with issues. */
	ZpoolList zpl;

	for (ZpoolList::iterator pool = zpl.begin(); pool != zpl.end(); pool++)
		VdevIterator(*pool).Each(VdevAddCaseFile, NULL);

	/* De-serialize any saved cases. */
	CaseFile::DeSerialize();
}

void
ZfsDaemon::RescanSystem()
{
        struct gmesh	  mesh;
        struct gclass	 *mp;
        struct ggeom 	 *gp;
        struct gprovider *pp;
	int		  result;

        /*
	 * The devctl system doesn't replay events for new consumers
	 * of the interface.  Emit manufactured DEVFS arrival events
	 * for any devices that already before we started or during
	 * periods where we've lost our connection to devd.
         */
	result = geom_gettree(&mesh);
	if (result != 0) {
		syslog(LOG_ERR, "ZfsDaemon::RescanSystem: "
		       "geom_gettree faild with error %d\n", result);
		return;
	}

	const string evStart("!system=DEVFS subsystem=CDEV type=CREATE "
			     "sub_type=synthesized cdev=");
        LIST_FOREACH(mp, &mesh.lg_class, lg_class) {
                LIST_FOREACH(gp, &mp->lg_geom, lg_geom) {
                        LIST_FOREACH(pp, &gp->lg_provider, lg_provider) {
				DevCtlEvent *event;

				string evString(evStart + pp->lg_name + "\n");
				event = DevCtlEvent::CreateEvent(evString);
				if (event != NULL)
					event->Process();
					delete event;
                        }
                }
	}
	geom_deletetree(&mesh);
}

void
ZfsDaemon::DetectMissedEvents()
{
	do {
		PurgeCaseFiles();

		/*
		 * Discard any events waiting for us.  We don't know
		 * if they still apply to the current state of the
		 * system.
		 */
		FlushEvents();

		BuildCaseFiles();

		/*
		 * If the system state has changed durring our
		 * interrogation, start over.
		 */
	} while (EventsPending());

	RescanSystem();
}

void
ZfsDaemon::EventLoop()
{
	EventBuffer eventBuffer(*s_reader);

	while (s_terminateEventLoop == false) {
		struct pollfd fds[2];
		int	      result;

		if (s_logCaseFiles == true) {
			s_logCaseFiles = false;
			CaseFile::LogAll();
		}

		Callout::ExpireCallouts();

		/* Wait for data. */
		fds[0].fd      = s_devdSockFD;
		fds[0].events  = POLLIN;
		fds[0].revents = 0;
		fds[1].fd      = s_signalPipeFD[0];
		fds[1].events  = POLLIN;
		fds[1].revents = 0;
		result = poll(fds, NUM_ELEMENTS(fds), /*timeout*/INFTIM);
		if (result == -1) {
			if (errno == EINTR)
				continue;
			else
				err(1, "Polling for devd events failed");
		} else if (result == 0) {
			errx(1, "Unexpected result of 0 from poll. Exiting");
		}

		if ((fds[0].revents & POLLIN) != 0)
			ProcessEvents(eventBuffer);

		if ((fds[1].revents & POLLIN) != 0) {
			static char discardBuf[128];

			/*
			 * This pipe exists just to close the signal
			 * race.  Its contents are of no interest to
			 * us, but we must ensure that future signals
			 * have space in the pipe to write.
			 */
			while (read(s_signalPipeFD[0], discardBuf,
				    sizeof(discardBuf)) > 0)
				;
		}

		if (s_systemRescanRequested == true) {
			s_systemRescanRequested = false;
			RescanSystem();
		}

		if ((fds[0].revents & POLLERR) != 0) {
			syslog(LOG_INFO, "POLLERROR detected on devd socket.");
			break;
		}

		if ((fds[0].revents & POLLHUP) != 0) {
			syslog(LOG_INFO, "POLLHUP detected on devd socket.");
			break;
		}
	}
}
