/*-
 * Copyright (c) 2011, 2012, 2013 Spectra Logic Corporation
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
 * \file event.cc
 *
 * Implementation of the class hierarchy used to express events
 * received via the devctl API.
 */
#include <sys/cdefs.h>
#include <sys/disk.h>
#include <sys/filio.h>
#include <sys/param.h>

#include <fcntl.h>
#include <paths.h>
#include <syslog.h>

#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <string>

#include "guid.h"
#include "event.h"
#include "event_factory.h"
#include "exception.h"

__FBSDID("$FreeBSD$");

/*================================== Macros ==================================*/
#define NUM_ELEMENTS(x) (sizeof(x) / sizeof(*x))

/*============================ Namespace Control =============================*/
using std::cout;
using std::endl;
using std::string;
using std::stringstream;

namespace DevCtl
{

/*=========================== Class Implementations ==========================*/
/*----------------------------------- Event ----------------------------------*/
//- Event Static Protected Data ------------------------------------------------
const string Event::s_theEmptyString;

Event::EventTypeRecord Event::s_typeTable[] =
{
	{ Event::NOTIFY,  "Notify" },
	{ Event::NOMATCH, "No Driver Match" },
	{ Event::ATTACH,  "Attach" },
	{ Event::DETACH,  "Detach" }
};

//- Event Static Public Methods ------------------------------------------------
Event *
Event::EventBuilder(Event::Type type, NVPairMap &nvPairs,
		    const string &eventString)
{
	return (new Event(type, nvPairs, eventString));
}

Event *
Event::CreateEvent(const EventFactory &factory, const string &eventString)
{
	NVPairMap &nvpairs(*new NVPairMap);
	Type       type(static_cast<Event::Type>(eventString[0]));

	try {
		ParseEventString(type, eventString, nvpairs);
	} catch (const ParseException &exp) {
		if (exp.GetType() == ParseException::INVALID_FORMAT)
			exp.Log();
		return (NULL);
	}

	/*
	 * Allow entries in our table for events with no system specified.
	 * These entries should specify the string "none".
	 */
	NVPairMap::iterator system_item(nvpairs.find("system"));
	if (system_item == nvpairs.end())
		nvpairs["system"] = "none";

	return (factory.Build(type, nvpairs, eventString));
}

const char *
Event::TypeToString(Event::Type type)
{
	EventTypeRecord *rec(s_typeTable);
	EventTypeRecord *lastRec(s_typeTable + NUM_ELEMENTS(s_typeTable) - 1);

	for (; rec <= lastRec; rec++) {
		if (rec->m_type == type)
			return (rec->m_typeName);
	}
	return ("Unknown");
}

//- Event Public Methods -------------------------------------------------------
const string &
Event::Value(const string &varName) const
{
	NVPairMap::const_iterator item(m_nvPairs.find(varName));
	if (item == m_nvPairs.end())
		return (s_theEmptyString);

	return (item->second);
}

bool
Event::Contains(const string &varName) const
{
	return (m_nvPairs.find(varName) != m_nvPairs.end());
}

string
Event::ToString() const
{
	stringstream result;

	NVPairMap::const_iterator devName(m_nvPairs.find("device-name"));
	if (devName != m_nvPairs.end())
		result << devName->second << ": ";

	NVPairMap::const_iterator systemName(m_nvPairs.find("system"));
	if (systemName != m_nvPairs.end()
	 && systemName->second != "none")
		result << systemName->second << ": ";

	result << TypeToString(GetType()) << ' ';

	for (NVPairMap::const_iterator curVar = m_nvPairs.begin();
	     curVar != m_nvPairs.end(); curVar++) {
		if (curVar == devName || curVar == systemName)
			continue;

		result << ' '
		     << curVar->first << "=" << curVar->second;
	}
	result << endl;

	return (result.str());
}

void
Event::Print() const
{
	cout << ToString() << std::flush;
}

void
Event::Log(int priority) const
{
	syslog(priority, "%s", ToString().c_str());
}

//- Event Virtual Public Methods -----------------------------------------------
Event::~Event()
{
	delete &m_nvPairs;
}

Event *
Event::DeepCopy() const
{
	return (new Event(*this));
}

bool
Event::Process() const
{
	return (false);
}

timeval
Event::GetTimestamp() const
{
	timeval tv_timestamp;
	struct tm tm_timestamp;

	if (!Contains("timestamp")) {
		throw Exception("Event contains no timestamp: %s",
				m_eventString.c_str());
	}
	strptime(Value(string("timestamp")).c_str(), "%s", &tm_timestamp);
	tv_timestamp.tv_sec = mktime(&tm_timestamp);
	tv_timestamp.tv_usec = 0;
	return (tv_timestamp);
}


//- Event Protected Methods ----------------------------------------------------
Event::Event(Type type, NVPairMap &map, const string &eventString)
 : m_type(type),
   m_nvPairs(map),
   m_eventString(eventString)
{
}

Event::Event(const Event &src)
 : m_type(src.m_type),
   m_nvPairs(*new NVPairMap(src.m_nvPairs)),
   m_eventString(src.m_eventString)
{
}

void
Event::ParseEventString(Event::Type type,
			      const string &eventString,
			      NVPairMap& nvpairs)
{
	size_t start;
	size_t end;

	switch (type) {
	case ATTACH:
	case DETACH:

		/*
		 * <type><device-name><unit> <pnpvars> \
		 *                        at <location vars> <pnpvars> \
		 *                        on <parent>
		 *
		 * Handle all data that doesn't conform to the
		 * "name=value" format, and let the generic parser
		 * below handle the rest.
		 *
		 * Type is a single char.  Skip it.
		 */
		start = 1;
		end = eventString.find_first_of(" \t\n", start);
		if (end == string::npos)
			throw ParseException(ParseException::INVALID_FORMAT,
					     eventString, start);

		nvpairs["device-name"] = eventString.substr(start, end - start);

		start = eventString.find(" on ", end);
		if (end == string::npos)
			throw ParseException(ParseException::INVALID_FORMAT,
					     eventString, start);
		start += 4;
		end = eventString.find_first_of(" \t\n", start);
		nvpairs["parent"] = eventString.substr(start, end);
		break;
	case NOTIFY:
		break;
	case NOMATCH:
		throw ParseException(ParseException::DISCARDED_EVENT_TYPE,
				     eventString);
	default:
		throw ParseException(ParseException::UNKNOWN_EVENT_TYPE,
				     eventString);
	}

	/* Process common "key=value" format. */
	for (start = 1; start < eventString.length(); start = end + 1) {

		/* Find the '=' in the middle of the key/value pair. */
		end = eventString.find('=', start);
		if (end == string::npos)
			break;

		/*
		 * Find the start of the key by backing up until
		 * we hit whitespace or '!' (event type "notice").
		 * Due to the devctl format, all key/value pair must
		 * start with one of these two characters.
		 */
		start = eventString.find_last_of("! \t\n", end);
		if (start == string::npos)
			throw ParseException(ParseException::INVALID_FORMAT,
					     eventString, end);
		start++;
		string key(eventString.substr(start, end - start));

		/*
		 * Walk forward from the '=' until either we exhaust
		 * the buffer or we hit whitespace.
		 */
		start = end + 1;
		if (start >= eventString.length())
			throw ParseException(ParseException::INVALID_FORMAT,
					     eventString, end);
		end = eventString.find_first_of(" \t\n", start);
		if (end == string::npos)
			end = eventString.length() - 1;
		string value(eventString.substr(start, end - start));

		nvpairs[key] = value;
	}
}

/*-------------------------------- DevfsEvent --------------------------------*/
//- DevfsEvent Static Public Methods -------------------------------------------
Event *
DevfsEvent::DevfsEventBuilder(Event::Type type, NVPairMap &nvPairs,
			      const string &eventString)
{
	return (new DevfsEvent(type, nvPairs, eventString));
}

//- DevfsEvent Static Protected Methods ----------------------------------------
bool
DevfsEvent::IsDiskDev(const string &devName)
{
	static const char *diskDevNames[] =
	{
		"da",
		"ada"
	};

	const char **diskName(diskDevNames);
	const char **lastDiskName(diskDevNames
				+ NUM_ELEMENTS(diskDevNames) - 1);
	size_t find_start = devName.rfind('/');
	if (find_start == string::npos) {
		find_start = 0;
	} else {
		/* Just after the last '/'. */
		find_start++;
	}

	for (; diskName <= lastDiskName; diskName++) {

		size_t loc(devName.find(*diskName, find_start));
		if (loc == find_start) {
			size_t prefixLen(strlen(*diskName));

			if (devName.length() - find_start >= prefixLen
			 && isdigit(devName[find_start + prefixLen]))
				return (true);
		}
	}

	return (false);
}

bool
DevfsEvent::IsWholeDev(const string &devName)
{
	string::const_iterator i(devName.begin());

	size_t start = devName.rfind('/');
	if (start == string::npos) {
		start = 0;
	} else {
		/* Just after the last '/'. */
		start++;
	}
	i += start;

	/* alpha prefix followed only by digits. */
	for (; i < devName.end() && !isdigit(*i); i++)
		;

	if (i == devName.end())
		return (false);

	for (; i < devName.end() && isdigit(*i); i++)
		;

	return (i == devName.end());
}

//- DevfsEvent Virtual Public Methods ------------------------------------------
Event *
DevfsEvent::DeepCopy() const
{
	return (new DevfsEvent(*this));
}

bool
DevfsEvent::Process() const
{
	return (true);
}

//- DevfsEvent Public Methods --------------------------------------------------
bool
DevfsEvent::IsDiskDev() const
{
	string devName;

	return (DevName(devName) && IsDiskDev(devName));
}

bool
DevfsEvent::IsWholeDev() const
{
	string devName;

	return (DevName(devName) && IsDiskDev() && IsWholeDev(devName));
}

bool
DevfsEvent::DevName(std::string &name) const
{
	if (Value("subsystem") != "CDEV")
		return (false);

	name = Value("cdev");
	return (!name.empty());
}

bool
DevfsEvent::DevPath(std::string &path) const
{
	string devName;

	if (!DevName(devName))
		return (false);

	string devPath(_PATH_DEV + devName);
	int devFd(open(devPath.c_str(), O_RDONLY));
	if (devFd == -1)
		return (false);

	/* Normalize the device name in case the DEVFS event is for a link. */
	devName = fdevname(devFd);
	path = _PATH_DEV + devName;

	close(devFd);

	return (true);
}

bool
DevfsEvent::PhysicalPath(std::string &path) const
{
	string devPath;

	if (!DevPath(devPath))
		return (false);

	int devFd(open(devPath.c_str(), O_RDONLY));
	if (devFd == -1)
		return (false);
	
	char physPath[MAXPATHLEN];
	physPath[0] = '\0';
	bool result(ioctl(devFd, DIOCGPHYSPATH, physPath) == 0);
	close(devFd);
	if (result)
		path = physPath;
	return (result);
}

//- DevfsEvent Protected Methods -----------------------------------------------
DevfsEvent::DevfsEvent(Event::Type type, NVPairMap &nvpairs,
		       const string &eventString)
 : Event(type, nvpairs, eventString)
{
}

DevfsEvent::DevfsEvent(const DevfsEvent &src)
 : Event(src)
{
}

/*--------------------------------- ZfsEvent ---------------------------------*/
//- ZfsEvent Static Public Methods ---------------------------------------------
Event *
ZfsEvent::ZfsEventBuilder(Event::Type type, NVPairMap &nvpairs,
			  const string &eventString)
{
	return (new ZfsEvent(type, nvpairs, eventString));
}

//- ZfsEvent Virtual Public Methods --------------------------------------------
Event *
ZfsEvent::DeepCopy() const
{
	return (new ZfsEvent(*this));
}

//- ZfsEvent Protected Methods -------------------------------------------------
ZfsEvent::ZfsEvent(Event::Type type, NVPairMap &nvpairs,
		   const string &eventString)
 : Event(type, nvpairs, eventString),
   m_poolGUID(Guid(Value("pool_guid"))),
   m_vdevGUID(Guid(Value("vdev_guid")))
{
}

ZfsEvent::ZfsEvent(const ZfsEvent &src)
 : Event(src),
   m_poolGUID(src.m_poolGUID),
   m_vdevGUID(src.m_vdevGUID)
{
}

} // namespace DevCtl
