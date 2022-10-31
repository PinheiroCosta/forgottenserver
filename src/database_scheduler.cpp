// Copyright 2022 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "database_scheduler.h"

#include "network_scheduler.h"

extern NetworkScheduler g_networkScheduler;

void DatabaseScheduler::start()
{
	db.connect();
	ThreadHolder::start();
}

void DatabaseScheduler::run()
{
	std::unique_lock<std::mutex> taskLockUnique(taskLock, std::defer_lock);
	while (getState() != THREAD_STATE_TERMINATED) {
		taskLockUnique.lock();
		if (taskList.empty()) {
			taskSignal.wait(taskLockUnique);
		}

		if (!taskList.empty()) {
			DatabaseTask task = std::move(taskList.front());
			taskList.pop_front();
			taskLockUnique.unlock();
			runTask(task);
		} else {
			taskLockUnique.unlock();
		}
	}
}

void DatabaseScheduler::addTask(std::string query, std::function<void(DBResult_ptr, bool)> callback /* = nullptr*/,
                                bool store /* = false*/)
{
	bool signal = false;
	taskLock.lock();
	if (getState() == THREAD_STATE_RUNNING) {
		signal = taskList.empty();
		taskList.emplace_back(std::move(query), std::move(callback), store);
	}
	taskLock.unlock();

	if (signal) {
		taskSignal.notify_one();
	}
}

void DatabaseScheduler::runTask(const DatabaseTask& task)
{
	bool success;
	DBResult_ptr result;
	if (task.store) {
		result = db.storeQuery(task.query);
		success = true;
	} else {
		result = nullptr;
		success = db.executeQuery(task.query);
	}

	if (task.callback) {
		g_networkScheduler.addTask([=, callback = task.callback]() { callback(result, success); });
	}
}

void DatabaseScheduler::flush()
{
	std::unique_lock<std::mutex> guard{taskLock};
	while (!taskList.empty()) {
		auto task = std::move(taskList.front());
		taskList.pop_front();
		guard.unlock();
		runTask(task);
		guard.lock();
	}
}

void DatabaseScheduler::shutdown()
{
	taskLock.lock();
	ThreadHolder::shutdown();
	taskLock.unlock();
	flush();
	taskSignal.notify_one();
}