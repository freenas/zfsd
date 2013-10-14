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
 *
 * $FreeBSD$
 */

/**
 * \file devctl_consumer.h
 */
#ifndef	_DEVCTL_CONSUMER_H_
#define	_DEVCTL_CONSUMER_H_

/*============================ Namespace Control =============================*/
namespace DevCtl
{

/*=========================== Forward Declarations ===========================*/
class Event;
class EventBuffer;
class FDReader;

/*============================ Class Declarations ============================*/
/*----------------------------- DevCtl::Consumer -----------------------------*/

/**
 */
class Consumer
{
public:
	Consumer(Event::BuildMethod *defBuilder = NULL,
		 EventFactory::Record *regEntries = NULL,
		 size_t numEntries = 0);
	virtual ~Consumer();

	bool Connected() const;

	/**
	 * Return file descriptor useful for client's wishing to poll(2)
	 * for new events.
	 */
	int GetPollFd();

	/**                                                          
         * Queue an event for deferred processing or replay.
         */ 
	bool SaveEvent(const Event &event);

	/**                                  
	 * Reprocess any events saved via the SaveEvent() facility.   
	 *
	 * \param discardUnconsumed  If true, events that are not conumed
	 *                           during replay are discarded.
	 */                                                              
	void ReplayUnconsumedEvents(bool discardUnconsumed);

	/** Return an event, if available, from the provided EventBuffer. */
	Event *NextEvent(EventBuffer *eventBuffer = NULL);

	/**
	 * Extract events from the provided eventBuffer and invoke
	 * each event's Process method.
	 */
	void ProcessEvents(EventBuffer *eventBuffer = NULL);

	/** Discard all data pending in m_devdSockFD. */
	void FlushEvents();

	/**
	 * Test for data pending in m_devdSockFD
	 *
	 * \return  True if data is pending.  Otherwise false.
	 */
	bool EventsPending();

	/**
	 * Open a connection to devd's unix domain socket.
	 *
	 * \return  True if the connection attempt is successsful.  Otherwise
	 *          false.
	 */
	bool ConnectToDevd();

	/**
	 * Close a connection (if any) to devd's unix domain socket.
	 */
	void DisconnectFromDevd();

	EventFactory GetFactory();

protected:
	static const char  s_devdSockPath[];

	/**
	 * File descriptor representing the unix domain socket
	 * connection with devd.
	 */
	int                m_devdSockFD;

	/**
	 * Reader tied to the devd socket.
	 */
	FDReader	  *m_reader;

	/**
	 * Default EventBuffer connected to m_reader.
	 */
	EventBuffer	  *m_eventBuffer;

	EventFactory	   m_eventFactory;

	/** Queued events for replay. */
	EventList	   m_unconsumedEvents;

	/**                                                             
	 * Flag controlling whether events can be queued.  This boolean
	 * is set during event replay to ensure that previosuly deferred
	 * events are not requeued and thus retained forever.
	 */
	bool		   m_replayingEvents;
};

//- Consumer Const Public Inline Methods ---------------------------------------
inline bool
Consumer::Connected() const
{
	return (m_devdSockFD != -1);
}

//- Consumer Public Inline Methods ---------------------------------------------
inline int
Consumer::GetPollFd()
{
	return (m_devdSockFD);
}

inline EventFactory
Consumer::GetFactory()
{
	return (m_eventFactory);
}

} // namespace DevCtl
#endif	/* _DEVCTL_CONSUMER_H_ */