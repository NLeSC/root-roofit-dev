#include "RooTimer.h"
#include "RooTrace.h"

// for debugging:
#include <iostream>
#include "unistd.h"

double RooTimer::timing_s() {
  return _timing_s;
}

void RooTimer::set_timing_s(double timing_s) {
  _timing_s = timing_s;
}

void RooTimer::store_timing_in_RooTrace(const std::string &name) {
//  std::cout << "pid in store_timing_in_RooTrace: " << getpid() << ". objectTiming size before insert: " << RooTrace::objectTiming.size() << std::endl;
//  RooTrace::objectTiming.insert({name, _timing_s});
//  std::cout << "pid in store_timing_in_RooTrace: " << getpid() << ". objectTiming size after insert: " << RooTrace::objectTiming.size() << std::endl;
  RooTrace::objectTiming[name] = _timing_s;  // subscript operator overwrites existing values, insert does not
}


RooWallTimer::RooWallTimer() {
  start();
}

void RooWallTimer::start() {
  _timing_begin = std::chrono::high_resolution_clock::now();
}

void RooWallTimer::stop() {
  _timing_end = std::chrono::high_resolution_clock::now();
  set_timing_s(std::chrono::duration_cast<std::chrono::nanoseconds>(_timing_end - _timing_begin).count() / 1.e9);
}


RooCPUTimer::RooCPUTimer() {
  start();
}

void RooCPUTimer::start() {
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &_timing_begin);
}

void RooCPUTimer::stop() {
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &_timing_end);
  set_timing_s((_timing_end.tv_nsec - _timing_begin.tv_nsec) / 1.e9);
}
