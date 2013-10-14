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
 * \file case_file.cc
 *
 * We keep case files for any leaf vdev that is not in the optimal state.
 * However, we only serialize to disk those events that need to be preserved
 * across reboots.  For now, this is just a log of soft errors which we
 * accumulate in order to mark a device as degraded.
 */
#include <dirent.h>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <syslog.h>
#include <unistd.h>

#include "case_file.h"
#include "vdev.h"
#include "zfsd.h"
#include "zfsd_exception.h"
#include "zpool_list.h"

/*============================ Namespace Control =============================*/
using std::auto_ptr;
using std::hex;
using std::ifstream;
using std::stringstream;
using std::setfill;
using std::setw;

/*--------------------------------- CaseFile ---------------------------------*/
//- CaseFile Static Data -------------------------------------------------------
CaseFileList  CaseFile::s_activeCases;
const string  CaseFile::s_caseFilePath = "/etc/zfs/cases";
const timeval CaseFile::s_removeGracePeriod = { 60 /*sec*/, 0 /*usec*/};

//- CaseFile Static Public Methods ---------------------------------------------

CaseFile *
CaseFile::Find(Guid poolGUID, Guid vdevGUID)
{
	for (CaseFileList::iterator curCase = s_activeCases.begin();
	     curCase != s_activeCases.end(); curCase++) {

		if ((*curCase)->PoolGUID() != poolGUID
		 || (*curCase)->VdevGUID() != vdevGUID)
			continue;

		/*
		 * We only carry one active case per-vdev.
		 */
		return (*curCase);
	}
	return (NULL);
}

CaseFile *
CaseFile::Find(const string &physPath)
{
	for (CaseFileList::iterator curCase = s_activeCases.begin();
	     curCase != s_activeCases.end(); curCase++) {

		if ((*curCase)->PhysicalPath() != physPath)
			continue;

		return (*curCase);
	}
	return (NULL);
}

CaseFile &
CaseFile::Create(Vdev &vdev)
{
	CaseFile *activeCase;

	activeCase = Find(vdev.PoolGUID(), vdev.GUID());
	if (activeCase == NULL)
		activeCase = new CaseFile(vdev);

	return (*activeCase);
}

void
CaseFile::DeSerialize()
{
	struct dirent **caseFiles;

	int numCaseFiles(scandir(s_caseFilePath.c_str(), &caseFiles,
			 DeSerializeSelector, /*compar*/NULL));

	if (numCaseFiles == -1)
		return;
	if (numCaseFiles == 0) {
		free(caseFiles);
		return;
	}

	for (int i = 0; i < numCaseFiles; i++) {

		DeSerializeFile(caseFiles[i]->d_name);
		free(caseFiles[i]);
	}
	free(caseFiles);
}

void
CaseFile::LogAll()
{
	for (CaseFileList::iterator curCase = s_activeCases.begin();
	     curCase != s_activeCases.end(); curCase++)
		(*curCase)->Log();
}

void
CaseFile::PurgeAll()
{
	/*
	 * Serialize casefiles before deleting them so that they can be reread
	 * and revalidated during BuildCaseFiles.
	 * CaseFiles remove themselves from this list on destruction.
	 */
	while (s_activeCases.size() != 0) {
		CaseFile *casefile = s_activeCases.front();
		casefile->Serialize();
		delete casefile;
	}

}

//- CaseFile Public Methods ----------------------------------------------------
bool
CaseFile::RefreshVdevState()
{
	ZpoolList zpl(ZpoolList::ZpoolByGUID, &m_poolGUID);
	if (zpl.empty()) {
		stringstream msg;
		msg << "CaseFile::RefreshVdevState: Unknown pool for Vdev(";
		msg << m_poolGUID << "," << m_vdevGUID << ").";
		syslog(LOG_INFO, "%s", msg.str().c_str());
			return (false);
	}

	zpool_handle_t *casePool(zpl.front());
	nvlist_t       *vdevConfig = VdevIterator(casePool).Find(VdevGUID());
	if (vdevConfig == NULL) {
		stringstream msg;
		syslog(LOG_INFO,
		       "CaseFile::RefreshVdevState: Unknown Vdev(%s,%s).\n",
		       PoolGUIDString().c_str(), PoolGUIDString().c_str());
		return (false);
	}
	Vdev caseVdev(casePool, vdevConfig);

	m_vdevState    = caseVdev.State();
	m_vdevPhysPath = caseVdev.PhysicalPath();
	return (true);
}

bool
CaseFile::ReEvaluate(const string &devPath, const string &physPath, Vdev *vdev)
{
	ZpoolList zpl(ZpoolList::ZpoolByGUID, &m_poolGUID);

	if (zpl.empty() || !RefreshVdevState()) {
		/*
		 * The pool or vdev for this case file is no longer
		 * part of the configuration.  This can happen
		 * if we process a device arrival notification
		 * before seeing the ZFS configuration change
		 * event.
		 */
		syslog(LOG_INFO,
		       "CaseFile::ReEvaluate(%s,%s) Pool/Vdev unconfigured.  "
		       "Closing\n", 
		       PoolGUIDString().c_str(),
		       VdevGUIDString().c_str());
		Close();

		/*
		 * Since this event was not used to close this
		 * case, do not report it as consumed.
		 */
		return (/*consumed*/false);
	}
	zpool_handle_t *pool(zpl.front());

	if (VdevState() > VDEV_STATE_CANT_OPEN) {
		/*
		 * For now, newly discovered devices only help for
		 * devices that are missing.  In the future, we might
		 * use a newly inserted spare to replace a degraded
		 * or faulted device.
		 */
		return (/*consumed*/false);
	}

	if (vdev != NULL
	 && vdev->PoolGUID() == m_poolGUID
	 && vdev->GUID() == m_vdevGUID) {

		zpool_vdev_online(pool, vdev->GUIDString().c_str(),
				  ZFS_ONLINE_CHECKREMOVE | ZFS_ONLINE_UNSPARE,
				  &m_vdevState);
		syslog(LOG_INFO, "Onlined vdev(%s/%s:%s).  State now %s.\n",
		       zpool_get_name(pool), vdev->GUIDString().c_str(),
		       devPath.c_str(),
		       zpool_state_to_name(VdevState(), VDEV_AUX_NONE));

		/*
		 * Check the vdev state post the online action to see
		 * if we can retire this case.
		 */
		CloseIfSolved();

		return (/*consumed*/true);
	}

	/*
	 * If the auto-replace policy is enabled, and we have physical
	 * path information, try a physical path replacement.
	 */
	if (zpool_get_prop_int(pool, ZPOOL_PROP_AUTOREPLACE, NULL) == 0) {
		syslog(LOG_INFO,
		       "CaseFile(%s:%s:%s): AutoReplace not set.  "
		       "Ignoring device insertion.\n",
		       PoolGUIDString().c_str(),
		       VdevGUIDString().c_str(),
		       zpool_state_to_name(VdevState(), VDEV_AUX_NONE));
		return (/*consumed*/false);
	}

	if (PhysicalPath().empty()) {
		syslog(LOG_INFO,
		       "CaseFile(%s:%s:%s): No physical path information.  "
		       "Ignoring device insertion.\n",
		       PoolGUIDString().c_str(),
		       VdevGUIDString().c_str(),
		       zpool_state_to_name(VdevState(), VDEV_AUX_NONE));
		return (/*consumed*/false);
	}

	if (physPath != PhysicalPath()) {
		syslog(LOG_INFO,
		       "CaseFile(%s:%s:%s): Physical path mismatch.  "
		       "Ignoring device insertion.\n",
		       PoolGUIDString().c_str(),
		       VdevGUIDString().c_str(),
		       zpool_state_to_name(VdevState(), VDEV_AUX_NONE));
		return (/*consumed*/false);
	}

	/* Write a label on the newly inserted disk. */
	if (zpool_label_disk(g_zfsHandle, pool, devPath.c_str()) != 0) {
		syslog(LOG_ERR,
		       "Replace vdev(%s/%s) by physical path (label): %s: %s\n",
		       zpool_get_name(pool), VdevGUIDString().c_str(),
		       libzfs_error_action(g_zfsHandle),
		       libzfs_error_description(g_zfsHandle));
		return (/*consumed*/false);
	}

	return (Replace(VDEV_TYPE_DISK, devPath.c_str()));
}

bool
CaseFile::ReEvaluate(const ZfsEvent &event)
{
	bool consumed(false);

	if (!RefreshVdevState()) {
		/*
		 * The pool or vdev for this case file is no longer
		 * part of the configuration.  This can happen
		 * if we process a device arrival notification
		 * before seeing the ZFS configuration change
		 * event.
		 */
		syslog(LOG_INFO,
		       "CaseFile::ReEvaluate(%s,%s) Pool/Vdev unconfigured.  "
		       "Closing\n", 
		       PoolGUIDString().c_str(),
		       VdevGUIDString().c_str());
		Close();

		/*
		 * Since this event was not used to close this
		 * case, do not report it as consumed.
		 */
		return (/*consumed*/false);
	}

	if (event.Value("type") == "misc.fs.zfs.vdev_remove") {
		/*
		 * The Vdev we represent has been removed from the
		 * configuration.  This case is no longer of value.
		 */
		Close();

		return (/*consumed*/true);
	}

	if (event.Value("class") == "resource.fs.zfs.removed") {
		bool spare_activated;

		/*
		 * Discard any tentative I/O error events for
		 * this case.  They were most likely caused by the
		 * hot-unplug of this device.
		 */
		PurgeTentativeEvents();

		/* Try to activate spares if they are available */
		spare_activated = ActivateSpare();

		/*
		 * Rescan the drives in the system to see if a recent
		 * drive arrival can be used to solve this case.
		 */
		ZfsDaemon::RequestSystemRescan();

		/* 
		 * Consume the event if we successfully activated a spare.
		 * Otherwise, leave it in the unconsumed events list so that the
		 * future addition of a spare to this pool might be able to
		 * close the case
		 */
		consumed = spare_activated;
	} else if (event.Value("class") == "ereport.fs.zfs.io"
		|| event.Value("class") == "ereport.fs.zfs.checksum") {

		m_tentativeEvents.push_front(event.DeepCopy());
		RegisterCallout(event);
		consumed = true;
	}

	bool closed(CloseIfSolved());

	return (consumed || closed);
}


bool
CaseFile::ActivateSpare() {
	nvlist_t	*config, *nvroot;
	nvlist_t       **spares;
	zpool_handle_t	*zhp;
	char		*devPath, *vdev_type;
	const char	*poolname;
	u_int		 nspares, i;
	int		 error;

	ZpoolList zpl(ZpoolList::ZpoolByGUID, &m_poolGUID);
	if (zpl.empty()) {
		syslog(LOG_ERR, "CaseFile::ActivateSpare: Could not find pool "
		       "for pool_guid %"PRIu64".", (uint64_t)m_poolGUID);
		return (false);
	}
	zhp = zpl.front();
	poolname = zpool_get_name(zhp);
	config = zpool_get_config(zhp, NULL);
	if (config == NULL) {
		syslog(LOG_ERR, "CaseFile::ActivateSpare: Could not find pool "
		       "config for pool %s", poolname);
		return (false);
	}
	if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, &nvroot) != 0){
		syslog(LOG_ERR, "CaseFile::ActivateSpare: Could not find vdev "
		       "tree for pool %s", poolname);
		return (false);
	}
	nspares = 0;
	nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_SPARES, &spares,
				   &nspares);
	if (nspares == 0) {
		/* The pool has no spares configured */
		return (false);
	}
	for (i = 0; i < nspares; i++) {
		uint64_t    *nvlist_array;
		vdev_stat_t *vs;
		uint_t	     nstats;

		if (nvlist_lookup_uint64_array(spares[i],
		    ZPOOL_CONFIG_VDEV_STATS, &nvlist_array, &nstats) != 0) {
			syslog(LOG_ERR, "CaseFile::ActivateSpare: Could not "
			       "find vdev stats for pool %s, spare %d",
			       poolname, i);
			return (false);
		}
		vs = reinterpret_cast<vdev_stat_t *>(nvlist_array);

		if ((vs->vs_aux != VDEV_AUX_SPARED)
		 && (vs->vs_state == VDEV_STATE_HEALTHY)) {
			/* We found a usable spare */
			break;
		}
	}

	if (i == nspares) {
		/* No available spares were found */
		return (false);
	}

	error = nvlist_lookup_string(spares[i], ZPOOL_CONFIG_PATH, &devPath);
	if (error != 0) {
		syslog(LOG_ERR, "CaseFile::ActivateSpare: Cannot determine "
		       "the path of pool %s, spare %d. Error %d",
		       poolname, i, error);
		return (false);
	}

	error = nvlist_lookup_string(spares[i], ZPOOL_CONFIG_TYPE, &vdev_type);
	if (error != 0) {
		syslog(LOG_ERR, "CaseFile::ActivateSpare: Cannot determine "
		       "the vdev type of pool %s, spare %d. Error %d",
		       poolname, i, error);
		return (false);
	}

	return (Replace(vdev_type, devPath));
}

void
CaseFile::RegisterCallout(const DevCtlEvent &event)
{   
	timeval now, countdown, elapsed, timestamp, zero, remaining;

	gettimeofday(&now, 0);
	timestamp = event.GetTimestamp();
	timersub(&now, &timestamp, &elapsed);
	timersub(&s_removeGracePeriod, &elapsed, &countdown);
	/*
	 * If countdown is <= zero, Reset the timer to the
	 * smallest positive time value instead
	 */
	timerclear(&zero);
	if (timercmp(&countdown, &zero, <=)) {
		timerclear(&countdown);
		countdown.tv_usec = 1;
	}

	remaining = m_tentativeTimer.TimeRemaining();

	if (!m_tentativeTimer.IsPending()
	 || timercmp(&countdown, &remaining, <))
		m_tentativeTimer.Reset(countdown, OnGracePeriodEnded, this);
}


bool
CaseFile::CloseIfSolved()
{
	if (m_events.empty()
	 && m_tentativeEvents.empty()) {

		/* 
		 * We currently do not track or take actions on
		 * devices in the degraded or faulted state.
		 * Once we have support for spare pools, we'll
		 * retain these cases so that any spares added in
		 * the future can be applied to them.
		 */
		if (VdevState() > VDEV_STATE_CANT_OPEN
		 && VdevState() <= VDEV_STATE_HEALTHY) {
			Close();
			return (true);
		}

		/*
		 * Re-serialize the case in order to remove any
		 * previous event data.
		 */
		Serialize();
	}

	return (false);
}

void
CaseFile::Log()
{
	syslog(LOG_INFO, "CaseFile(%s,%s,%s)\n", PoolGUIDString().c_str(),
	       VdevGUIDString().c_str(), PhysicalPath().c_str());
	syslog(LOG_INFO, "\tVdev State = %s\n",
	       zpool_state_to_name(VdevState(), VDEV_AUX_NONE));
	if (m_tentativeEvents.size() != 0) {
		syslog(LOG_INFO, "\t=== Tentative Events ===\n");
		for (DevCtlEventList::iterator event(m_tentativeEvents.begin());
		     event != m_tentativeEvents.end(); event++)
			(*event)->Log(LOG_INFO);
	}
	if (m_events.size() != 0) {
		syslog(LOG_INFO, "\t=== Events ===\n");
		for (DevCtlEventList::iterator event(m_events.begin());
		     event != m_events.end(); event++)
			(*event)->Log(LOG_INFO);
	}
}

//- CaseFile Static Protected Methods ------------------------------------------
void
CaseFile::OnGracePeriodEnded(void *arg)
{
	CaseFile &casefile(*static_cast<CaseFile *>(arg));

	casefile.OnGracePeriodEnded();
}

int
CaseFile::DeSerializeSelector(const struct dirent *dirEntry)
{
	uint64_t poolGUID;
	uint64_t vdevGUID;

	if (dirEntry->d_type == DT_REG
	 && sscanf(dirEntry->d_name, "pool_%"PRIu64"_vdev_%"PRIu64".case",
		   &poolGUID, &vdevGUID) == 2)
		return (1);
	return (0);
}

void
CaseFile::DeSerializeFile(const char *fileName)
{
	string	  fullName(s_caseFilePath + '/' + fileName);
	CaseFile *existingCaseFile(NULL);
	CaseFile *caseFile(NULL);

	try {
		uint64_t poolGUID;
		uint64_t vdevGUID;
		nvlist_t *vdevConf;

		sscanf(fileName, "pool_%"PRIu64"_vdev_%"PRIu64".case",
		       &poolGUID, &vdevGUID);
		existingCaseFile = Find(Guid(poolGUID), Guid(vdevGUID));
		if (existingCaseFile != NULL) {
			/*
			 * If the vdev is already degraded or faulted,
			 * there's no point in keeping the state around
			 * that we use to put a drive into the degraded
			 * state.  However, if the vdev is simply missing,
			 * preseve the case data in the hopes that it will
			 * return.
			 */
			caseFile = existingCaseFile;
			vdev_state curState(caseFile->VdevState());
			if (curState > VDEV_STATE_CANT_OPEN
			 && curState < VDEV_STATE_HEALTHY) {
				unlink(fileName);
				return;
			}
		} else {
			ZpoolList zpl(ZpoolList::ZpoolByGUID, &poolGUID);
			if (zpl.empty()
			 || (vdevConf = VdevIterator(zpl.front())
						    .Find(vdevGUID)) == NULL) {
				/*
				 * Either the pool no longer exists
				 * or this vdev is no longer a member of
				 * the pool.
				 */
				unlink(fullName.c_str());
				return;
			}

			/*
			 * Any vdev we find that does not have a case file
			 * must be in the healthy state and thus worthy of
			 * continued SERD data tracking.
			 */
			caseFile = new CaseFile(Vdev(zpl.front(), vdevConf));
		}

		ifstream caseStream(fullName.c_str());
		if (!caseStream)
			throw ZfsdException("CaseFile::DeSerialize: Unable to "
					    "read %s.\n", fileName);

		caseFile->DeSerialize(caseStream);
	} catch (const ParseException &exp) {

		exp.Log();
		if (caseFile != existingCaseFile)
			delete caseFile;

		/*
		 * Since we can't parse the file, unlink it so we don't
		 * trip over it again.
		 */
		unlink(fileName);
	} catch (const ZfsdException &zfsException) {

		zfsException.Log();
		if (caseFile != existingCaseFile)
			delete caseFile;
	}
}

//- CaseFile Protected Methods -------------------------------------------------
CaseFile::CaseFile(const Vdev &vdev)
 : m_poolGUID(vdev.PoolGUID()),
   m_vdevGUID(vdev.GUID()),
   m_vdevState(vdev.State()),
   m_vdevPhysPath(vdev.PhysicalPath())
{
	stringstream guidString;

	guidString << m_vdevGUID;
	m_vdevGUIDString = guidString.str();
	guidString.str("");
	guidString << m_poolGUID;
	m_poolGUIDString = guidString.str();

	s_activeCases.push_back(this);

	syslog(LOG_INFO, "Creating new CaseFile:\n");
	Log();
}

CaseFile::~CaseFile()
{
	PurgeEvents();
	PurgeTentativeEvents();
	m_tentativeTimer.Stop();
	s_activeCases.remove(this);
}

void
CaseFile::PurgeEvents()
{
	for (DevCtlEventList::iterator event(m_events.begin());
	     event != m_events.end(); event++)
		delete *event;

	m_events.clear();
}

void
CaseFile::PurgeTentativeEvents()
{
	for (DevCtlEventList::iterator event(m_tentativeEvents.begin());
	     event != m_tentativeEvents.end(); event++)
		delete *event;

	m_tentativeEvents.clear();
}

void
CaseFile::SerializeEvList(const DevCtlEventList events, int fd,
		const char* prefix) const
{
	if (events.empty())
		return;
	for (DevCtlEventList::const_iterator curEvent = events.begin();
	     curEvent != events.end(); curEvent++) {
		const string &eventString((*curEvent)->GetEventString());

		if (prefix)
			write(fd, prefix, strlen(prefix));
		write(fd, eventString.c_str(), eventString.length());
	}
}

void
CaseFile::Serialize()
{
	stringstream saveFile;

	saveFile << setfill('0')
		 << s_caseFilePath << "/"
		 << "pool_" << PoolGUIDString()
		 << "_vdev_" << VdevGUIDString()
		 << ".case";

	if (m_events.empty() && m_tentativeEvents.empty()) {
		unlink(saveFile.str().c_str());
		return;
	}

	int fd(open(saveFile.str().c_str(), O_CREAT|O_TRUNC|O_WRONLY, 0644));
	if (fd == -1) {
		syslog(LOG_ERR, "CaseFile::Serialize: Unable to open %s.\n",
		       saveFile.str().c_str());
		return;
	}
	SerializeEvList(m_events, fd);
	SerializeEvList(m_tentativeEvents, fd, "tentative ");
	close(fd);
}

void
CaseFile::DeSerialize(ifstream &caseStream)
{
	stringstream  fakeDevdSocket(stringstream::in|stringstream::out);
	IstreamReader caseReader(&fakeDevdSocket);
	EventBuffer   eventBuffer(caseReader);
	string	      evString;

	caseStream >> std::noskipws >> std::ws;
	while (!caseStream.eof()) {
		/*
		 * Outline:
		 * read the beginning of a line and check it for
		 * "tentative".  If found, discard "tentative".
		 * Shove into fakeDevdSocket.
		 * call ExtractEvent
		 * continue
		 */
		DevCtlEventList* destEvents;
		string tentFlag("tentative ");
		string line;
		std::stringbuf lineBuf;

		caseStream.get(lineBuf);
		caseStream.ignore();  /*discard the newline character*/
		line = lineBuf.str();
		if (line.compare(0, tentFlag.size(), tentFlag) == 0) {
			line.erase(0, tentFlag.size());
			destEvents = &m_tentativeEvents;
		} else {
			destEvents = &m_events;
		}
		fakeDevdSocket << line;
		fakeDevdSocket << '\n';
		while (eventBuffer.ExtractEvent(evString)) {
			DevCtlEvent *event(DevCtlEvent::CreateEvent(evString));
			if (event != NULL) {
				destEvents->push_back(event);
				RegisterCallout(*event);
			}
		}
	}
}

void
CaseFile::Close()
{
	/*
	 * This case is no longer relevant.  Clean up our
	 * serialization file, and delete the case.
	 */
	syslog(LOG_INFO, "CaseFile(%s,%s) closed - State %s\n",
	       PoolGUIDString().c_str(), VdevGUIDString().c_str(),
	       zpool_state_to_name(VdevState(), VDEV_AUX_NONE));
	
	/*
	 * Serialization of a Case with no event data, clears the
	 * Serialization data for that event.
	 */
	PurgeEvents();
	Serialize();

	delete this;
}

void
CaseFile::OnGracePeriodEnded()
{
	m_events.splice(m_events.begin(), m_tentativeEvents);

	if (m_events.size() > ZFS_DEGRADE_IO_COUNT) {

		ZpoolList zpl(ZpoolList::ZpoolByGUID, &m_poolGUID);
		if (zpl.empty()
		 || (VdevIterator(zpl.front()).Find(m_vdevGUID)) == NULL) {
			/*
			 * Either the pool no longer exists
			 * or this vdev is no longer a member of
			 * the pool.
			 */
			Close();
			return;
		}

		/* Degrade the vdev and close the case. */
		if (zpool_vdev_degrade(zpl.front(), (uint64_t)m_vdevGUID,
				       VDEV_AUX_ERR_EXCEEDED) == 0) {
			syslog(LOG_INFO, "Degrading vdev(%s/%s)",
			       PoolGUIDString().c_str(),
			       VdevGUIDString().c_str()); 
			Close();
			return;
		}
		else {
			syslog(LOG_ERR, "Degrade vdev(%s/%s): %s: %s\n",
			       PoolGUIDString().c_str(),
			       VdevGUIDString().c_str(),
			       libzfs_error_action(g_zfsHandle),
			       libzfs_error_description(g_zfsHandle));
		}
	}
	Serialize();
}

bool
CaseFile::Replace(const char* vdev_type, const char* path) {
	nvlist_t *nvroot, *newvd;
	zpool_handle_t *zhp;
	const char* poolname;

	/* Figure out what pool we're working on */
	ZpoolList zpl(ZpoolList::ZpoolByGUID, &m_poolGUID);
	if (zpl.empty()) {
		syslog(LOG_ERR, "CaseFile::Replace: could not find pool for "
		       "pool_guid %"PRIu64".", (uint64_t)m_poolGUID);
		return (false);
	}
	zhp = zpl.front();
	poolname = zpool_get_name(zhp);

	/*
	 * Build a root vdev/leaf vdev configuration suitable for
	 * zpool_vdev_attach. Only enough data for the kernel to find
	 * the device (i.e. type and disk device node path) are needed.
	 */
	nvroot = NULL;
	newvd = NULL;

	if (nvlist_alloc(&nvroot, NV_UNIQUE_NAME, 0) != 0
	 || nvlist_alloc(&newvd, NV_UNIQUE_NAME, 0) != 0) {
		syslog(LOG_ERR, "Replace vdev(%s/%s) by physical path: "
		       "Unable to allocate configuration data.\n",
		       poolname, VdevGUIDString().c_str());
		if (nvroot != NULL)
			nvlist_free(nvroot);
		return (false);
	}
	if (nvlist_add_string(newvd, ZPOOL_CONFIG_TYPE, vdev_type) != 0
	 || nvlist_add_string(newvd, ZPOOL_CONFIG_PATH, path) != 0
	 || nvlist_add_string(nvroot, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT) != 0
	 || nvlist_add_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
				    &newvd, 1) != 0) {
		syslog(LOG_ERR, "Replace vdev(%s/%s) by physical path: "
		       "Unable to initialize configuration data.\n",
		       poolname, VdevGUIDString().c_str());
		nvlist_free(newvd);
		nvlist_free(nvroot);
		return (true);
	}

	/* Data was copied when added to the root vdev. */
	nvlist_free(newvd);

	if (zpool_vdev_attach(zhp, VdevGUIDString().c_str(),
			      path, nvroot, /*replace*/B_TRUE) != 0) {
		syslog(LOG_ERR,
		       "Replace vdev(%s/%s) by physical path(attach): %s: %s\n",
		       poolname, VdevGUIDString().c_str(),
		       libzfs_error_action(g_zfsHandle),
		       libzfs_error_description(g_zfsHandle));
	} else {
		syslog(LOG_INFO, "Replacing vdev(%s/%s) with %s\n",
		       poolname, VdevGUIDString().c_str(),
		       path);
	}
	nvlist_free(nvroot);

	return (true);
}
