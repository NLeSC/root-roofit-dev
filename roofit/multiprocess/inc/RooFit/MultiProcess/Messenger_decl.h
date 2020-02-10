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
#ifndef ROOT_ROOFIT_MultiProcess_Messenger_decl
#define ROOT_ROOFIT_MultiProcess_Messenger_decl

#include <iosfwd>
#include <vector>
#include <csignal>  // sigprocmask, sigset_t, etc

#include "RooFit_ZMQ/ZeroMQSvc.h"
#include "RooFit_ZMQ/ZeroMQPoller.h"
#include "RooFit/MultiProcess/ProcessManager.h"

namespace RooFit {
namespace MultiProcess {

void set_socket_immediate(ZmqLingeringSocketPtr<> &socket);

// test messages
enum class X2X : int {
   ping = -1,
   pong = -2,
};

class Messenger {
public:
   explicit Messenger(const ProcessManager &process_manager);
   ~Messenger();

   void test_connections(const ProcessManager &process_manager);

   enum class test_snd_pipes {
      M2Q,
      Q2M,
      Q2W,
      W2Q
   };

   enum class test_rcv_pipes {
      fromQonM,
      fromMonQ,
      fromWonQ,
      fromQonW,
   };

   void close_master_queue_connection(bool close_context) noexcept;
   void close_queue_worker_connections(bool close_context);

   std::pair<ZeroMQPoller, std::size_t> create_queue_poller();
   ZeroMQPoller create_worker_poller();

   // -- WORKER - QUEUE COMMUNICATION --

   void send_from_worker_to_queue();
   template <typename T, typename... Ts>
   void send_from_worker_to_queue(T item, Ts... items);
   template <typename value_t>
   value_t receive_from_worker_on_queue(std::size_t this_worker_id);
   void send_from_queue_to_worker(std::size_t this_worker_id);
   template <typename T, typename... Ts>
   void send_from_queue_to_worker(std::size_t this_worker_id, T item, Ts... items);
   template <typename value_t>
   value_t receive_from_queue_on_worker();

   // -- QUEUE - MASTER COMMUNICATION --

   void send_from_queue_to_master();

   template <typename T, typename... Ts>
   void send_from_queue_to_master(T item, Ts... items);
   template <typename value_t>
   value_t receive_from_queue_on_master();
   void send_from_master_to_queue();

   template <typename T, typename... Ts>
   void send_from_master_to_queue(T item, Ts... items);
   template <typename value_t>
   value_t receive_from_master_on_queue();

//   bool is_initialized() const;

   void test_receive(ZmqLingeringSocketPtr<> &socket, X2X expected_ping_value, test_rcv_pipes rcv_pipe, std::size_t worker_id);
   void test_send(ZmqLingeringSocketPtr<> &socket, X2X ping_value, test_snd_pipes snd_pipe, std::size_t worker_id);

   sigset_t ppoll_sigmask;
//   std::size_t N_available_polled_results = 0;

private:
   // push
   std::vector<ZmqLingeringSocketPtr<>> qw_push;
   ZmqLingeringSocketPtr<> this_worker_qw_push;
   ZmqLingeringSocketPtr<> mq_push;
   // pollers for all push sockets
   std::vector<ZeroMQPoller> qw_push_poller;
   ZeroMQPoller mq_push_poller;
   // pull
   std::vector<ZmqLingeringSocketPtr<>> qw_pull;
   ZmqLingeringSocketPtr<> this_worker_qw_pull;
   ZmqLingeringSocketPtr<> mq_pull;
   // pollers for all pull sockets
   std::vector<ZeroMQPoller> qw_pull_poller;
   ZeroMQPoller mq_pull_poller;
};


// Messages from master to queue
enum class M2Q : int {
   terminate = 100,
   enqueue = 10,
   retrieve = 11,
   update_real = 12,
   //      update_cat = 13,
};

// Messages from queue to master
enum class Q2M : int { retrieve_rejected = 20, retrieve_accepted = 21, retrieve_later = 22 };

// Messages from worker to queue
enum class W2Q : int { dequeue = 30, send_result = 31 };

// Messages from queue to worker
enum class Q2W : int {
   terminate = 400,
   dequeue_rejected = 40,
   dequeue_accepted = 41,
   result_received = 43,
   update_real = 44,
   //      update_cat = 45
};

// stream output operators for debugging
std::ostream &operator<<(std::ostream &out, const M2Q value);
std::ostream &operator<<(std::ostream &out, const Q2M value);
std::ostream &operator<<(std::ostream &out, const Q2W value);
std::ostream &operator<<(std::ostream &out, const W2Q value);

} // namespace MultiProcess
} // namespace RooFit

#endif // ROOT_ROOFIT_MultiProcess_Messenger_decl
