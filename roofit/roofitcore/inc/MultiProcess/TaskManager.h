/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 * @(#)root/roofitcore:$Id$
 * Authors:                                                                  *
 *   PB, Patrick Bos,     NL eScience Center, p.bos@esciencecenter.nl        *
 *   IP, Inti Pelupessy,  NL eScience Center, i.pelupessy@esciencecenter.nl  *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/

#ifndef ROOFIT_MULTIPROCESS_TASKMANAGER_H
#define ROOFIT_MULTIPROCESS_TASKMANAGER_H

#include <string>
#include <map>
#include <queue>
#include <MultiProcess/BidirMMapPipe.h>
// N.B.: cannot use forward declarations for BidirMMapPipe, because we need
// the nested BidirMMapPipe::PollVector as well.

namespace RooFit {
  namespace MultiProcess {

    // forward declarations
    class Job;
    enum class M2Q: int;
    enum class W2Q: int;

    // some helper types
    using Task = std::size_t;
    using JobTask = std::pair<std::size_t, Task>;  // combined job_object and task identifier type

    // TaskManager handles message passing and communication with a queue of
    // tasks and workers that execute the tasks. The queue is in a separate
    // process that can communicate with the master process (from where this
    // object is created) and the queue process communicates with the worker
    // processes.
    //
    // The TaskManager class does work defined by subclasses of the Job class.
    //
    // For message passing, enum class T based on int are used. The implementer
    // must make sure that T can be sent over the BidirMMapPipe, i.e. that
    // operator<<(BidirMMapPipe&, T) and >>(BidirMMapPipe&, T) are implemented.
    // This is currently done in messages.cxx/.h, see examples there.
    //
    // Make sure that activate() is called soon after instantiation of TaskManager,
    // because everything in between construction and activate() gets executed
    // on all processes (master, queue and slaves). Activate starts the queue
    // loop on the queue process, which means it can start doing its job.
    // Worker processes have to be activated separately from the Job objects
    // themselves. Activate cannot be called from inside the constructor, since
    // the loops would prevent the constructor from returning a constructed
    // object (thus defying its purpose). Note that at the end of activate, the
    // queue and child processes are killed. This is achieved by sending the
    // terminate message, which is done automatically in the destructor of this
    // class, but can also be done manually via terminate().
    //
    // When using everything as intended, i.e. by only instantiating via the
    // instance() method, activate() is called from Job::get_manager()
    // immediately after creation, so one need not worry about the above.
    class TaskManager {
     public:
      static std::size_t get_N_workers();
      static void set_N_workers(std::size_t _N_workers);
      static bool instance_created();

      static std::shared_ptr <TaskManager> instance();

      void identify_processes();

      explicit TaskManager(std::size_t N_workers);

      ~TaskManager();

      static std::size_t add_job_object(Job *job_object);

      static Job *get_job_object(std::size_t job_object_id);

      static bool remove_job_object(std::size_t job_object_id);

      void terminate();

      void terminate_workers();

      void activate();

      bool is_activated();

      BidirMMapPipe::PollVector get_poll_vector();

      bool process_queue_pipe_message(M2Q message);

      void retrieve();

      void process_worker_pipe_message(BidirMMapPipe &pipe, std::size_t this_worker_id, W2Q message);

      void queue_loop();

      bool from_queue(JobTask &job_task);

      void to_queue(JobTask job_task);

      bool is_master();

      bool is_queue();

      bool is_worker();

      void set_work_mode(bool flag);

      std::shared_ptr <BidirMMapPipe> &get_worker_pipe();

      std::size_t get_worker_id();

      std::shared_ptr <BidirMMapPipe> &get_queue_pipe();

//      std::map<JobTask, double>& get_results();
      double call_double_const_method(std::string method_key, std::size_t job_id, std::size_t worker_id_call);

      void send_from_worker_to_queue();

      void send_from_queue_to_master();

      template<typename T, typename ... Ts>
      void send_from_worker_to_queue(T item, Ts ... items) {
        *this_worker_pipe << item;
//      if (sizeof...(items) > 0) {  // this will only work with if constexpr, c++17
        send_from_worker_to_queue(items...);
      }

      template <typename value_t>
      value_t receive_from_worker_on_queue(std::size_t this_worker_id) {
        value_t value;
        *worker_pipes[this_worker_id] >> value;
        return value;
      }

      template<typename T, typename ... Ts>
      void send_from_queue_to_master(T item, Ts ... items) {
        *queue_pipe << item;
//      if (sizeof...(items) > 0) {  // this will only work with if constexpr, c++17
        send_from_queue_to_master(items...);
      }

      template <typename value_t>
      value_t receive_from_queue_on_master() {
        value_t value;
        *queue_pipe >> value;
        return value;
      }

     private:
      std::vector <std::shared_ptr<BidirMMapPipe>> worker_pipes;
      // for convenience on the worker processes, we use this_worker_pipe as an
      // alias for worker_pipes.back()
      std::shared_ptr <BidirMMapPipe> this_worker_pipe;
      std::shared_ptr <BidirMMapPipe> queue_pipe;
      std::size_t worker_id;
      bool _is_master = false;
      bool _is_queue = false;
      std::queue <JobTask> queue;
      std::size_t N_tasks = 0;  // total number of received tasks
      std::size_t N_tasks_completed = 0;
      bool queue_activated = false;
      bool work_mode = false;

      static std::map<std::size_t, Job *> job_objects;
      static std::size_t job_counter;
      static std::weak_ptr <TaskManager> _instance;
      static std::size_t N_workers;
    };

  } // namespace MultiProcess
} // namespace RooFit

#endif //ROOFIT_MULTIPROCESS_TASKMANAGER_H
