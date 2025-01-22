/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "chunkserver/network_worker_thread.h"

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "chunkserver/bgjobs.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/network_stats.h"
#include "common/event_loop.h"
#include "common/massert.h"
#include "slogger/slogger.h"
#include "common/sockets.h"
#include "devtools/request_log.h"
#include "devtools/TracePrinter.h"

// connection timeout in seconds
constexpr uint32_t kCSServTimeout = 10;

constexpr int kTimeoutOdd = 300000;
constexpr int kTimeoutEven = 200000;

constexpr int getConnectTimeout(int cnt) {
	return cnt % 2 ? kTimeoutOdd * (1 << (cnt >> 1))
	               : kTimeoutEven * (1 << (cnt >> 1));
}

NetworkWorkerThread::NetworkWorkerThread(uint32_t nrOfBgjobsWorkers,
                                         uint32_t bgjobsCount)
    : doTerminate(false) {
	TRACETHIS();
	eassert(pipe(notify_pipe) != -1);
#ifdef F_SETPIPE_SZ
	// Increase the pipe size to 128 KiB to handle a larger number of jobs
	// without backpressure. On modern linux, the default pipe size is 64 KiB.
	static constexpr int kPageAlignedPipeSize = 4096 * 32;
	eassert(fcntl(notify_pipe[1], F_SETPIPE_SZ, kPageAlignedPipeSize));
#endif
	bgJobPool_ =
	    job_pool_new(nrOfBgjobsWorkers, bgjobsCount, &bgJobPoolWakeUpFd_);
}

void NetworkWorkerThread::operator()() {
	TRACETHIS();

	static std::atomic_uint16_t threadCounter(0);
	std::string threadName = "networkWorker " + std::to_string(threadCounter++);
	pthread_setname_np(pthread_self(), threadName.c_str());

	while (!doTerminate) {
		preparePollFds();
		int i = poll(pdesc.data(), pdesc.size(), gPollTimeout);
		if (i < 0) {
			if (errno == EAGAIN) {
				safs_pretty_syslog(LOG_WARNING, "poll returned EAGAIN");
				usleep(100000);
				continue;
			}
			if (errno != EINTR) {
				safs_pretty_syslog(LOG_WARNING, "poll error: %s",
				                   strerr(errno));
				break;
			}
		} else {
			if ((pdesc[0].revents) & POLLIN) {
				uint8_t notifyByte;
				eassert(read(pdesc[0].fd, &notifyByte, 1) == 1);
			}
		}
		servePoll();
	}
	this->terminate();
}

void NetworkWorkerThread::terminate() {
	TRACETHIS();
	job_pool_delete(bgJobPool_);

	std::unique_lock lock(csservheadLock);

	while (!csservEntries.empty()) {
		auto& entry = csservEntries.back();

		if (entry.isChunkOpen) {
			hddClose(entry.chunkId, entry.chunkType);
		}

		csservEntries.pop_back(); // Should call the entry destructor
	}
}

void NetworkWorkerThread::preparePollFds() {
	LOG_AVG_TILL_END_OF_SCOPE0("preparePollFds");
	TRACETHIS();
	pdesc.clear();
	pdesc.emplace_back(pollfd(notify_pipe[0], POLLIN, 0));
	pdesc.emplace_back(pollfd(bgJobPoolWakeUpFd_, POLLIN, 0));
	sassert(JOB_FD_PDESC_POS == (pdesc.size() - 1));

	std::unique_lock lock(csservheadLock);
	for (auto& entry : csservEntries) {
		entry.pDescPos = -1;
		entry.fwdPDescPos = -1;
		switch (entry.state) {
			case ChunkserverEntry::State::Idle:
			case ChunkserverEntry::State::Read:
			case ChunkserverEntry::State::GetBlock:
			case ChunkserverEntry::State::WriteLast:
				pdesc.emplace_back(pollfd(entry.sock, 0, 0));
				entry.pDescPos = pdesc.size() - 1;
				if (entry.inputPacket.bytesLeft > 0) {
					pdesc.back().events |= POLLIN;
				}
				if (!entry.outputPackets.empty()) {
					pdesc.back().events |= POLLOUT;
				}
				break;
			case ChunkserverEntry::State::Connecting:
				pdesc.emplace_back(pollfd(entry.fwdSocket, POLLOUT, 0));
				entry.fwdPDescPos = pdesc.size() - 1;
				break;
			case ChunkserverEntry::State::WriteInit:
				if (entry.fwdBytesLeft > 0) {
					pdesc.emplace_back(pollfd(entry.fwdSocket, POLLOUT, 0));
					entry.fwdPDescPos = pdesc.size() - 1;
				}
				break;
			case ChunkserverEntry::State::WriteForward:
				pdesc.emplace_back(pollfd(entry.fwdSocket, POLLIN, 0));
				entry.fwdPDescPos = pdesc.size() - 1;
				if (entry.fwdBytesLeft > 0) {
					pdesc.back().events |= POLLOUT;
				}

				pdesc.emplace_back(pollfd(entry.sock, 0, 0));
				entry.pDescPos = pdesc.size() - 1;
				if (entry.inputPacket.bytesLeft > 0) {
					pdesc.back().events |= POLLIN;
				}
				if (!entry.outputPackets.empty()) {
					pdesc.back().events |= POLLOUT;
				}
				break;
			case ChunkserverEntry::State::WriteFinish:
				if (!entry.outputPackets.empty()) {
					pdesc.emplace_back(pollfd(entry.sock, POLLOUT, 0));
					entry.pDescPos = pdesc.size() - 1;
				}
				break;
			default:
				break;
		}
	}
}

void NetworkWorkerThread::servePoll() {
	LOG_AVG_TILL_END_OF_SCOPE0("servePoll");
	TRACETHIS();
	uint32_t now = eventloop_time();
	uint64_t usecnow = eventloop_utime();
	uint32_t jobscnt;
	ChunkserverEntry::State lstate;

	if (pdesc[JOB_FD_PDESC_POS].revents & POLLIN) {
		job_pool_check_jobs(bgJobPool_);
	}
	std::unique_lock lock(csservheadLock);
	for (auto& entry : csservEntries) {
		ChunkserverEntry* eptr = &entry;
		if (entry.pDescPos >= 0
				&& (pdesc[entry.pDescPos].revents & (POLLERR | POLLHUP))) {
			entry.state = ChunkserverEntry::State::Close;
		} else if (entry.fwdPDescPos >= 0
				&& (pdesc[entry.fwdPDescPos].revents & (POLLERR | POLLHUP))) {
			eptr->fwdError();
		}
		lstate = entry.state;
		if (lstate == ChunkserverEntry::State::Idle ||
		    lstate == ChunkserverEntry::State::Read ||
		    lstate == ChunkserverEntry::State::WriteLast ||
		    lstate == ChunkserverEntry::State::WriteFinish ||
		    lstate == ChunkserverEntry::State::GetBlock) {
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLIN)) {
				entry.lastActivity = now;
				eptr->readFromSocket();
			}
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLOUT) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				eptr->writeToSocket();
			}
		} else if (lstate == ChunkserverEntry::State::Connecting &&
		           entry.fwdPDescPos >= 0 &&
		           (pdesc[entry.fwdPDescPos].revents &
		            POLLOUT)) {  // FD_ISSET(entry.fwdsock,wset)) {
			entry.lastActivity = now;
			eptr->fwdConnected();
			if (entry.state == ChunkserverEntry::State::WriteInit) {
				eptr->fwdWrite(); // after connect likely some data can be send
			}
			if (entry.state == ChunkserverEntry::State::WriteForward) {
				eptr->forward(); // and also some data can be forwarded
			}
		} else if (entry.state == ChunkserverEntry::State::WriteInit &&
		           entry.fwdPDescPos >= 0 &&
		           (pdesc[entry.fwdPDescPos].revents &
		            POLLOUT)) {  // FD_ISSET(entry.fwdsock,wset)) {
			entry.lastActivity = now;
			eptr->fwdWrite(); // after sending init packet
			if (entry.state == ChunkserverEntry::State::WriteForward) {
				eptr->forward(); // likely some data can be forwarded
			}
		} else if (entry.state == ChunkserverEntry::State::WriteForward) {
			if ((entry.pDescPos >= 0 &&
			     (pdesc[entry.pDescPos].revents & POLLIN)) ||
			    (entry.fwdPDescPos >= 0 &&
			     (pdesc[entry.fwdPDescPos].revents & POLLOUT))) {
				entry.lastActivity = now;
				eptr->forward();
			}
			if (entry.fwdPDescPos >= 0 &&
			    (pdesc[entry.fwdPDescPos].revents & POLLIN) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				eptr->fwdRead();
			}
			if (entry.pDescPos >= 0 &&
			    (pdesc[entry.pDescPos].revents & POLLOUT) &&
			    entry.state == lstate) {
				entry.lastActivity = now;
				eptr->writeToSocket();
			}
		}
		if (entry.state == ChunkserverEntry::State::WriteFinish &&
		    entry.outputPackets.empty()) {
			entry.state = ChunkserverEntry::State::Close;
		}
		if (entry.state == ChunkserverEntry::State::Connecting &&
		    entry.connectStartTimeUSec +
		            getConnectTimeout(entry.connectRetryCounter) <
		        usecnow) {
			eptr->retryConnect();
		}
		if (entry.state != ChunkserverEntry::State::Close &&
		    entry.state != ChunkserverEntry::State::CloseWait &&
		    entry.state != ChunkserverEntry::State::Closed &&
		    entry.lastActivity + kCSServTimeout < now) {
			// Close connection if inactive for more than kCSServTimeout seconds
			entry.state = ChunkserverEntry::State::Close;
		}
		if (entry.state == ChunkserverEntry::State::Close) {
			eptr->closeJobs();
		}
	}

	jobscnt = job_pool_jobs_count(bgJobPool_);
//      // Lock free stats_maxjobscnt = max(stats_maxjobscnt, jobscnt), but I don't trust myself :(...
//      uint32_t expected_value = stats_maxjobscnt;
//      while (jobscnt > expected_value
//                      && !stats_maxjobscnt.compare_exchange_strong(expected_value, jobscnt)) {
//              expected_value = stats_maxjobscnt;
//      }
// // .. Will end up with a racy code instead :(
	if (jobscnt > stats_maxjobscnt) {
		// A race is possible here, but it won't lead to any serious consequences, in a worst
		// (and unlikely) case stats_maxjobscnt will be slightly lower than it actually should be
		stats_maxjobscnt = jobscnt;
	}

	for (auto it = csservEntries.begin(); it != csservEntries.end();) {
		auto &eptr = *it;
		if (eptr.state == ChunkserverEntry::State::Closed) {
			it = csservEntries.erase(it); // Should call the entry destructor
		} else {
			++it;
		}
	}
}

void NetworkWorkerThread::askForTermination() {
	TRACETHIS();
	doTerminate = true;
}

void NetworkWorkerThread::addConnection(int newSocketFD) {
	TRACETHIS();
	tcpnonblock(newSocketFD);
	tcpnodelay(newSocketFD);

	std::unique_lock lock(csservheadLock);
	csservEntries.emplace_front(newSocketFD, bgJobPool_);
	csservEntries.front().lastActivity = eventloop_time();

	eassert(write(notify_pipe[1], "9", 1) == 1);
}
