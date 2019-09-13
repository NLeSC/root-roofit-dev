/*
 * Project: RooFit
 * Authors:
 *   PB, Patrick Bos, Netherlands eScience Center, p.bos@esciencecenter.nl
 *   IP, Inti Pelupessy, Netherlands eScience Center, i.pelupessy@esciencecenter.nl
 *
 * Copyright (c) 2016-2019, Netherlands eScience Center
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "RooFit/MultiProcess/JobManager.h"
#include "RooFit/MultiProcess/Messenger.h"
#include "RooFit/MultiProcess/worker.h"
#include "RooFit/MultiProcess/Job.h"

namespace RooFit {
namespace MultiProcess {

Job::Job(std::size_t _N_workers) : N_workers(_N_workers)
{
   id = JobManager::add_job_object(this);
}

Job::Job(const Job &other)
   : N_workers(other.N_workers), waiting_for_queued_tasks(other.waiting_for_queued_tasks), _manager(other._manager)
{
   id = JobManager::add_job_object(this);
}

Job::~Job()
{
   JobManager::remove_job_object(id);
}

// This function is necessary here, because the Job knows about the number
// of workers, so only from the Job can the JobManager be instantiated.
JobManager *Job::get_manager()
{
   if (!_manager) {
      _manager = JobManager::instance(N_workers);
   }

   _manager->activate();

   // N.B.: must check for queue activation here, otherwise get_manager is not callable
   //       from queue loop!
   if (!_manager->queue().is_activated()) {
      _manager->queue().activate();
   }

   if (!_manager->worker_loop_activated()) {
      _manager->activate_worker_loop();
   }

   return _manager;
}

void Job::gather_worker_results()
{
   if (waiting_for_queued_tasks) {
      get_manager()->retrieve();
      waiting_for_queued_tasks = false;
   }
}

} // namespace MultiProcess
} // namespace RooFit
