/*
 * chronos, the cron-job.org execution daemon
 * Copyright (C) 2017 Patrick Schlangen <patrick@schlangen.me>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include "WorkerThread.h"

#include <iostream>
#include <memory>
#include <stdexcept>

#include "UpdateThread.h"
#include "App.h"

#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/transport/TTransportUtils.h>

#include "ChronosMaster.h"

using namespace Chronos;

namespace {

int curlSocketFunction(CURL *e, curl_socket_t s, int what, void *userdata, void *sockp)
{
	return static_cast<WorkerThread *>(userdata)->socketFunction(e, s, what, static_cast<SockInfo *>(sockp));
}

int curlTimerFunction(CURLM *multi, long timeout_ms, void *userdata)
{
	return static_cast<WorkerThread *>(userdata)->timerFunction(multi, timeout_ms);
}

void evTimerFunction(EV_P_ struct ev_timer *w, int revents)
{
	static_cast<WorkerThread *>(w->data)->evTimerFunction(w, revents);
}

void evEventFunction(EV_P_ struct ev_io *w, int revents)
{
	static_cast<WorkerThread *>(w->data)->evEventFunction(w, revents);
}

}

WorkerThread::WorkerThread(int mday, int month, int year, int hour, int minute, std::size_t parallelJobs, std::size_t deferMs)
	: mday(mday), month(month), year(year), hour(hour), minute(minute), parallelJobs(parallelJobs), deferMs(deferMs)
{
}

WorkerThread::~WorkerThread()
{
}

void WorkerThread::checkResults()
{
	CURLMsg *msg;
	int msgsLeft;

	while((msg = curl_multi_info_read(curlHandle, &msgsLeft)))
	{
		if(msg->msg == CURLMSG_DONE)
		{
			CURL *easy = msg->easy_handle;
			CURLcode res = msg->data.result;

			HTTPRequest *req = nullptr;
			if(curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req) != CURLE_OK || req == nullptr)
				throw std::runtime_error("Failed to retrieve associated HTTPRequest!");

			req->done(res);
		}
	}
}

int WorkerThread::timerFunction(CURLM *multi, long timeout_ms)
{
	if(ev_is_active(&timerEvent))
	{
		// Short cut for the case were curl asks for lots of timeout_ms = 0 timers
		// when adding the easy handles to the multi handle in a loop. We want to avoid
		// calling ev_timer_stop(), ev_timer_set(), ev_timer_start() for each of those.
		if(timeout_ms == 0)
		{
			ev_tstamp remaining = ev_timer_remaining(evLoop, &timerEvent);
			if (remaining < 10e-3)
			{
				return 0;
			}
		}

		ev_timer_stop(evLoop, &timerEvent);
	}

	// Set timer
	if(timeout_ms >= 0)
	{
		double t = static_cast<double>(timeout_ms) / 1000.;
		ev_timer_set(&timerEvent, t, 0);
		ev_timer_start(evLoop, &timerEvent);
	}

	// Stop timer
	else if (timeout_ms == -1)
	{

	}

	return 0;
}

int WorkerThread::socketFunction(CURL *e, curl_socket_t s, int what, SockInfo *sockInfo)
{
	if(what == CURL_POLL_REMOVE)
	{
		removeSocket(sockInfo);
	}
	else
	{
		if(sockInfo == nullptr)
			addSocket(e, s, what);
		else
			setSocket(sockInfo, e, s, what);
	}
	return 0;
}

void WorkerThread::removeSocket(SockInfo *sockInfo)
{
	if(sockInfo->evset)
		ev_io_stop(evLoop, &sockInfo->ev);
	delete sockInfo;
}

void WorkerThread::addSocket(CURL *e, curl_socket_t s, int what)
{
	std::unique_ptr<SockInfo> sockInfo = std::make_unique<SockInfo>();
	sockInfo->evset = 0;
	setSocket(sockInfo.get(), e, s, what);
	curl_multi_assign(curlHandle, s, sockInfo.release());
}

void WorkerThread::setSocket(SockInfo *sockInfo, CURL *e, curl_socket_t s, int what)
{
	int kind = ((what & CURL_POLL_IN) ? EV_READ : 0) | ((what & CURL_POLL_OUT) ? EV_WRITE : 0);
	sockInfo->sockfd = s;
	sockInfo->action = what;
	sockInfo->easy = e;
	if(sockInfo->evset)
		ev_io_stop(evLoop, &sockInfo->ev);
	ev_io_init(&sockInfo->ev, ::evEventFunction, sockInfo->sockfd, kind);
	sockInfo->ev.data = this;
	sockInfo->evset = 1;
	ev_io_start(evLoop, &sockInfo->ev);
}

void WorkerThread::evTimerFunction(struct ev_timer *w, int revents)
{
	CURLMcode rc = curl_multi_socket_action(curlHandle, CURL_SOCKET_TIMEOUT, 0, &curlStillRunning);
	if(rc != CURLM_OK)
		throw std::runtime_error("curl_multi_socket_action failed (1)!");
	checkResults();
}

void WorkerThread::evEventFunction(struct ev_io *w, int revents)
{
	int action = (revents & EV_READ) ? CURL_POLL_IN : 0;
	action |= (revents & EV_WRITE) ? CURL_POLL_OUT : 0;
	CURLMcode rc = curl_multi_socket_action(curlHandle, w->fd, action, &curlStillRunning);
	if(rc != CURLM_OK)
		throw std::runtime_error("curl_multi_socket_action failed (2)!");
	checkResults();
	if(curlStillRunning <= 0)
		ev_timer_stop(evLoop, &timerEvent);
}

void WorkerThread::addJob(HTTPRequest *req)
{
	requestQueue.push(req);
}

void WorkerThread::run()
{
	if(requestQueue.empty())
		return;

	keepAlive = shared_from_this();

	workerThread = std::thread(std::bind(&WorkerThread::threadMain, this));
	workerThread.detach();
}

void WorkerThread::runJobs()
{
	if(inRunJobs)
		return;

	inRunJobs = true;

	while(runningJobs < parallelJobs && !requestQueue.empty())
	{
		HTTPRequest *job = requestQueue.front();
		requestQueue.pop();

		++runningJobs;

		job->onDone = std::bind(&WorkerThread::jobDone, this, job);
		job->submit(curlHandle);
	}

	inRunJobs = false;
}

void WorkerThread::jobDone(HTTPRequest *req)
{
	jitterSum += req->result->jitter;
	if(req->result->jitter > jitterMax)
	{
		jitterMax = req->result->jitter;
	}
	if(req->result->jitter < jitterMin)
	{
		jitterMin = req->result->jitter;
	}

	int timeTotalMs = req->result->timeTotal / 1000;
	timeTotalSum += timeTotalMs;
	if(timeTotalMs > timeTotalMax)
	{
		timeTotalMax = timeTotalMs;
	}
	if(timeTotalMs < timeTotalMin)
	{
		timeTotalMin = timeTotalMs;
	}

	if(req->result->status == JOBSTATUS_OK)
	{
		++succeededJobs;
	}
	else
	{
		++failedJobs;
	}

	// push result to result queue
	UpdateThread::getInstance()->addResult(std::move(req->result));

	// clean up
	delete req;
	if(runningJobs > 0)
		--runningJobs;

	// start more jobs
	runJobs();

	// exit event loop when all requests have finished
	if(runningJobs == 0)
		ev_break(evLoop, EVBREAK_ALL);
}

void WorkerThread::addStat()
{
	std::shared_ptr<apache::thrift::transport::TTransport> masterSocket
		= std::make_shared<apache::thrift::transport::TSocket>(
			App::getInstance()->config->get("master_service_address"),
			App::getInstance()->config->getInt("master_service_port"));
	std::shared_ptr<apache::thrift::transport::TTransport> masterTransport
		= std::make_shared<apache::thrift::transport::TBufferedTransport>(masterSocket);
	std::shared_ptr<apache::thrift::protocol::TProtocol> masterProtocol
		= std::make_shared<apache::thrift::protocol::TBinaryProtocol>(masterTransport);
	std::shared_ptr<ChronosMasterClient> masterClient
		= std::make_shared<ChronosMasterClient>(masterProtocol);

	try
	{
		masterTransport->open();

		const double jitterAvg = jitterSum / static_cast<double>(jobCount);
		const double timeTotalAvg = timeTotalSum / static_cast<double>(jobCount);
		const double failureRate = static_cast<double>(failedJobs) / static_cast<double>(jobCount);

		NodeStatsEntry stats;
		stats.d = mday;
		stats.m = month;
		stats.y = year;
		stats.h = hour;
		stats.i = minute;
		stats.jobs = jobCount;
		stats.jitter = jitterAvg;

		masterClient->reportNodeStats(App::getInstance()->config->getInt("node_id"),
			stats);

		masterTransport->close();

		std::cout << "WorkerThread::addStat(): mday = " << mday << ", month = " << month << ", hour = " << hour << ", minute = " << minute << ": "
			<< "jobCount = " << jobCount << ", succeededJobs = " << succeededJobs << ", failedJobs = " << failedJobs << " (" << (failureRate * 100.0) << " %), "
			<< "jitterMin = " << jitterMin << ", jitterMax = " << jitterMax << ", jitterAvg = " << jitterAvg << ", "
			<< "timeTotalMin = " << timeTotalMin << ", timeTotalMax = " << timeTotalMax << ", timeTotalAvg = " << timeTotalAvg << ", "
			<< std::endl;
	}
	catch(const apache::thrift::TException &ex)
	{
		std::cerr << "WorkerThread::addStat(): Failed to report node stats: " << ex.what() << std::endl;
	}
}

void WorkerThread::threadMain()
{
	try
	{
		std::cout << "WorkerThread::threadMain(): Entered" << std::endl;

		if(deferMs > 0)
		{
			std::cout << "WorkerThread::threadMain(): Deferring thread by " << deferMs << " ms..." << std::endl;
			usleep(deferMs * 1000);
		}

		jobCount = requestQueue.size();

		// init event loop
		evLoop = ev_loop_new(EVFLAG_AUTO);
		if(evLoop == nullptr)
			throw std::runtime_error("ev_loop_new() failed");

		curlHandle = curl_multi_init();
		if(curlHandle == nullptr)
			throw std::runtime_error("curl_multi_init() failed");

		ev_timer_init(&timerEvent, ::evTimerFunction, 0, 0);
		timerEvent.data = this;

		curl_multi_setopt(curlHandle, CURLMOPT_SOCKETFUNCTION,		::curlSocketFunction);
		curl_multi_setopt(curlHandle, CURLMOPT_SOCKETDATA,			this);
		curl_multi_setopt(curlHandle, CURLMOPT_TIMERFUNCTION,		::curlTimerFunction);
		curl_multi_setopt(curlHandle, CURLMOPT_TIMERDATA,			this);

		// add jobs
		runJobs();

		// main loop
		ev_loop(evLoop, 0);

		// clean up
		curl_multi_cleanup(curlHandle);
		ev_loop_destroy(evLoop);

		addStat();

		std::cout << "WorkerThread::threadMain(): Finished" << std::endl;
	}
	catch(const std::runtime_error &ex)
	{
		std::cout << "WorkerThread::threadEntry(): Exception: " << ex.what() << std::endl;
	}

	keepAlive.reset();
}
