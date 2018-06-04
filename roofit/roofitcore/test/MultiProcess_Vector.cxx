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

// MultiProcess back-end
#include <MultiProcess/BidirMMapPipe.h>
#include <MultiProcess/messages.h>
#include <MultiProcess/TaskManager.h>
#include <MultiProcess/Vector.h>

// MultiProcess parallelized RooFit classes
#include <MultiProcess/NLLVar.h>
#include <MultiProcess/GradMinimizer.h>

#include <cstdlib>  // std::_Exit
#include <cmath>
#include <vector>
#include <map>
#include <exception>
#include <numeric> // accumulate
#include <tuple>   // for google test Combine in parameterized test

#include <RooRealVar.h>
#include <RooRandom.h>

// for NLL tests
#include <RooWorkspace.h>
#include <RooAbsPdf.h>
#include <RooDataSet.h>
#include <RooMinimizer.h>
#include <RooGradMinimizer.h>
#include <RooFitResult.h>

#include "gtest/gtest.h"

class xSquaredPlusBVectorSerial {
 public:
  xSquaredPlusBVectorSerial(double b, std::vector<double> x_init) :
      _b("b", "b", b),
      x(std::move(x_init)),
      result(x.size()) {}

  virtual void evaluate() {
    // call evaluate_task for each task
    for (std::size_t ix = 0; ix < x.size(); ++ix) {
      result[ix] = std::pow(x[ix], 2) + _b.getVal();
    }
  }

  std::vector<double> get_result() {
    evaluate();
    return result;
  }

 protected:
  RooRealVar _b;
  std::vector<double> x;
  std::vector<double> result;
};


using RooFit::MultiProcess::JobTask;

class xSquaredPlusBVectorParallel : public RooFit::MultiProcess::Vector<xSquaredPlusBVectorSerial> {
 public:
  xSquaredPlusBVectorParallel(std::size_t NumCPU, double b_init, std::vector<double> x_init) :
      RooFit::MultiProcess::Vector<xSquaredPlusBVectorSerial>(NumCPU, b_init,
           x_init) // NumCPU stands for everything that defines the parallelization behaviour (number of cpu, strategy, affinity etc)
  {}

  void evaluate() override {
    if (get_manager()->is_master()) {
      // start work mode
      get_manager()->set_work_mode(true);

      // master fills queue with tasks
      retrieved = false;
      for (std::size_t task_id = 0; task_id < x.size(); ++task_id) {
        JobTask job_task(id, task_id);
        get_manager()->to_queue(job_task);
      }

      // wait for task results back from workers to master
      gather_worker_results();

      // end work mode
      get_manager()->set_work_mode(false);

      // put task results in desired container (same as used in serial class)
      for (std::size_t task_id = 0; task_id < x.size(); ++task_id) {
        result[task_id] = results[task_id];
      }
    }
  }


 private:
  void evaluate_task(std::size_t task) override {
    assert(get_manager()->is_worker());
    result[task] = std::pow(x[task], 2) + _b.getVal();
  }

  double get_task_result(std::size_t task) override {
    assert(get_manager()->is_worker());
    return result[task];
  }

};

class MultiProcessVectorSingleJob : public ::testing::TestWithParam<std::size_t> {
  // You can implement all the usual fixture class members here.
  // To access the test parameter, call GetParam() from class
  // TestWithParam<T>.
};


class Hex {
 public:
  explicit Hex(double n) : number_(n) {}
  operator double() const { return number_; }
  bool operator==(const Hex& other) {
    return double(*this) == double(other);
  }

 private:
  double number_;
};

::std::ostream& operator<<(::std::ostream& os, const Hex& hex) {
  return os << std::hexfloat << double(hex) << std::defaultfloat;  // whatever needed to print bar to os
}


TEST_P(MultiProcessVectorSingleJob, getResult) {
  // Simple test case: calculate x^2 + b, where x is a vector. This case does
  // both a simple calculation (squaring the input vector x) and represents
  // handling of state updates in b.
  std::vector<double> x{0, 1, 2, 3};
  double b_initial = 3.;

  // start serial test

  xSquaredPlusBVectorSerial x_sq_plus_b(b_initial, x);

  auto y = x_sq_plus_b.get_result();
  std::vector<double> y_expected{3, 4, 7, 12};

  EXPECT_EQ(Hex(y[0]), Hex(y_expected[0]));
  EXPECT_EQ(Hex(y[1]), Hex(y_expected[1]));
  EXPECT_EQ(Hex(y[2]), Hex(y_expected[2]));
  EXPECT_EQ(Hex(y[3]), Hex(y_expected[3]));

  std::size_t NumCPU = GetParam();

  // start parallel test

  xSquaredPlusBVectorParallel x_sq_plus_b_parallel(NumCPU, b_initial, x);

  auto y_parallel = x_sq_plus_b_parallel.get_result();

  EXPECT_EQ(Hex(y_parallel[0]), Hex(y_expected[0]));
  EXPECT_EQ(Hex(y_parallel[1]), Hex(y_expected[1]));
  EXPECT_EQ(Hex(y_parallel[2]), Hex(y_expected[2]));
  EXPECT_EQ(Hex(y_parallel[3]), Hex(y_expected[3]));
}


INSTANTIATE_TEST_CASE_P(NumberOfWorkerProcesses,
                        MultiProcessVectorSingleJob,
                        ::testing::Values(1,2,3));


class MultiProcessVectorMultiJob : public ::testing::TestWithParam<std::size_t> {};

TEST_P(MultiProcessVectorMultiJob, getResult) {
  // Simple test case: calculate x^2 + b, where x is a vector. This case does
  // both a simple calculation (squaring the input vector x) and represents
  // handling of state updates in b.
  std::vector<double> x{0, 1, 2, 3};
  double b_initial = 3.;

  std::vector<double> y_expected{3, 4, 7, 12};

  std::size_t NumCPU = GetParam();

  // define jobs
  xSquaredPlusBVectorParallel x_sq_plus_b_parallel(NumCPU, b_initial, x);
  xSquaredPlusBVectorParallel x_sq_plus_b_parallel2(NumCPU, b_initial + 1, x);

  // do stuff
  auto y_parallel = x_sq_plus_b_parallel.get_result();
  auto y_parallel2 = x_sq_plus_b_parallel2.get_result();

  EXPECT_EQ(Hex(y_parallel[0]), Hex(y_expected[0]));
  EXPECT_EQ(Hex(y_parallel[1]), Hex(y_expected[1]));
  EXPECT_EQ(Hex(y_parallel[2]), Hex(y_expected[2]));
  EXPECT_EQ(Hex(y_parallel[3]), Hex(y_expected[3]));

  EXPECT_EQ(Hex(y_parallel2[0]), Hex(y_expected[0] + 1));
  EXPECT_EQ(Hex(y_parallel2[1]), Hex(y_expected[1] + 1));
  EXPECT_EQ(Hex(y_parallel2[2]), Hex(y_expected[2] + 1));
  EXPECT_EQ(Hex(y_parallel2[3]), Hex(y_expected[3] + 1));
}


INSTANTIATE_TEST_CASE_P(NumberOfWorkerProcesses,
                        MultiProcessVectorMultiJob,
                        ::testing::Values(2,1,3));



TEST(MPFEnll, getVal) {
  // check whether MPFE produces the same results when using different NumCPU or mode.
  // this defines the baseline against which we compare our MP NLL
  RooRandom::randomGenerator()->SetSeed(3);
  // N.B.: it passes on seeds 1 and 2

  RooWorkspace w;
  w.factory("Gaussian::g(x[-5,5],mu[0,-3,3],sigma[1])");
  auto x = w.var("x");
  RooAbsPdf *pdf = w.pdf("g");
  RooRealVar *mu = w.var("mu");
  RooDataSet *data = pdf->generate(RooArgSet(*x), 10000);
  double results[4];

  RooArgSet values = RooArgSet(*mu, *pdf);

  auto nll1 = pdf->createNLL(*data, RooFit::NumCPU(1));
  results[0] = nll1->getVal();
  delete nll1;
  auto nll2 = pdf->createNLL(*data, RooFit::NumCPU(2));
  results[1] = nll2->getVal();
  delete nll2;
  auto nll3 = pdf->createNLL(*data, RooFit::NumCPU(3));
  results[2] = nll3->getVal();
  delete nll3;
  auto nll4 = pdf->createNLL(*data, RooFit::NumCPU(4));
  results[3] = nll4->getVal();
  delete nll4;
  auto nll1b = pdf->createNLL(*data, RooFit::NumCPU(1));
  auto result1b = nll1b->getVal();
  delete nll1b;
  auto nll2b = pdf->createNLL(*data, RooFit::NumCPU(2));
  auto result2b = nll2b->getVal();
  delete nll2b;

  auto nll1_mpfe = pdf->createNLL(*data, RooFit::NumCPU(-1));
  auto result1_mpfe = nll1_mpfe->getVal();
  delete nll1_mpfe;

  auto nll1_interleave = pdf->createNLL(*data, RooFit::NumCPU(1, 1));
  auto result_interleave1 = nll1_interleave->getVal();
  delete nll1_interleave;
  auto nll2_interleave = pdf->createNLL(*data, RooFit::NumCPU(2, 1));
  auto result_interleave2 = nll2_interleave->getVal();
  delete nll2_interleave;
  auto nll3_interleave = pdf->createNLL(*data, RooFit::NumCPU(3, 1));
  auto result_interleave3 = nll3_interleave->getVal();
  delete nll3_interleave;

  EXPECT_DOUBLE_EQ(Hex(results[0]), Hex(results[1]));
  EXPECT_DOUBLE_EQ(Hex(results[0]), Hex(results[2]));
  EXPECT_DOUBLE_EQ(Hex(results[0]), Hex(results[3]));

  EXPECT_DOUBLE_EQ(Hex(results[0]), Hex(result1b));
  EXPECT_DOUBLE_EQ(Hex(results[1]), Hex(result2b));
  EXPECT_DOUBLE_EQ(Hex(results[0]), Hex(result1_mpfe));

  EXPECT_DOUBLE_EQ(Hex(results[0]), Hex(result_interleave1));
  EXPECT_DOUBLE_EQ(Hex(results[0]), Hex(result_interleave2));
  EXPECT_DOUBLE_EQ(Hex(results[0]), Hex(result_interleave3));
}


class MultiProcessVectorNLL : public ::testing::TestWithParam<std::tuple<std::size_t, RooFit::MultiProcess::NLLVarTask, std::size_t>> {};


TEST_P(MultiProcessVectorNLL, getVal) {
  // Real-life test: calculate a NLL using event-based parallelization. This
  // should replicate RooRealMPFE results.
  RooRandom::randomGenerator()->SetSeed(std::get<2>(GetParam()));
  RooWorkspace w;
  w.factory("Gaussian::g(x[-5,5],mu[0,-3,3],sigma[1])");
  auto x = w.var("x");
  RooAbsPdf *pdf = w.pdf("g");
  RooDataSet *data = pdf->generate(RooArgSet(*x), 10000);
  auto nll = pdf->createNLL(*data);

  auto nominal_result = nll->getVal();

  std::size_t NumCPU = std::get<0>(GetParam());
  RooFit::MultiProcess::NLLVarTask mp_task_mode = std::get<1>(GetParam());

  RooFit::MultiProcess::NLLVar nll_mp(NumCPU, mp_task_mode, *dynamic_cast<RooNLLVar*>(nll));

  auto mp_result = nll_mp.getVal();

  EXPECT_DOUBLE_EQ(Hex(nominal_result), Hex(mp_result));
  if (HasFailure()) {
    std::cout << "failed test had parameters NumCPU = " << NumCPU << ", task_mode = " << mp_task_mode << ", seed = " << std::get<2>(GetParam()) << std::endl;
  }
}


TEST_P(MultiProcessVectorNLL, setVal) {
  // calculate the NLL twice with different parameters

  RooRandom::randomGenerator()->SetSeed(std::get<2>(GetParam()));
  RooWorkspace w;
  w.factory("Gaussian::g(x[-5,5],mu[0,-3,3],sigma[1])");
  auto x = w.var("x");
  RooAbsPdf *pdf = w.pdf("g");
  RooDataSet *data = pdf->generate(RooArgSet(*x), 10000);
  auto nll = pdf->createNLL(*data);

  std::size_t NumCPU = std::get<0>(GetParam());
  RooFit::MultiProcess::NLLVarTask mp_task_mode = std::get<1>(GetParam());

  RooFit::MultiProcess::NLLVar nll_mp(NumCPU, mp_task_mode, *dynamic_cast<RooNLLVar*>(nll));

  // calculate first results
  nll->getVal();
  nll_mp.getVal();

  w.var("mu")->setVal(2);

  // calculate second results after parameter change
  auto nominal_result2 = nll->getVal();
  auto mp_result2 = nll_mp.getVal();

  EXPECT_DOUBLE_EQ(Hex(nominal_result2), Hex(mp_result2));
  if (HasFailure()) {
    std::cout << "failed test had parameters NumCPU = " << NumCPU << ", task_mode = " << mp_task_mode << ", seed = " << std::get<2>(GetParam()) << std::endl;
  }
}


INSTANTIATE_TEST_CASE_P(NworkersModeSeed,
                        MultiProcessVectorNLL,
                        ::testing::Combine(::testing::Values(1,2,3),  // number of workers
                                           ::testing::Values(RooFit::MultiProcess::NLLVarTask::all_events,
                                                             RooFit::MultiProcess::NLLVarTask::single_event,
                                                             RooFit::MultiProcess::NLLVarTask::bulk_partition,
                                                             RooFit::MultiProcess::NLLVarTask::interleave),
                                           ::testing::Values(2,3)));  // random seed




class NLLMultiProcessVsMPFE : public ::testing::TestWithParam<std::tuple<std::size_t, RooFit::MultiProcess::NLLVarTask, std::size_t>> {};

TEST_P(NLLMultiProcessVsMPFE, getVal) {
  // Compare our MP NLL to actual RooRealMPFE results using the same strategies.

  // parameters
  std::size_t NumCPU = std::get<0>(GetParam());
  RooFit::MultiProcess::NLLVarTask mp_task_mode = std::get<1>(GetParam());
  std::size_t seed = std::get<2>(GetParam());

  RooRandom::randomGenerator()->SetSeed(seed);

  RooWorkspace w;
  w.factory("Gaussian::g(x[-5,5],mu[0,-3,3],sigma[1])");
  auto x = w.var("x");
  RooAbsPdf *pdf = w.pdf("g");
  RooDataSet *data = pdf->generate(RooArgSet(*x), 10000);

  int mpfe_task_mode = 0;
  if (mp_task_mode == RooFit::MultiProcess::NLLVarTask::interleave) {
    mpfe_task_mode = 1;
  }

  auto nll_mpfe = pdf->createNLL(*data, RooFit::NumCPU(NumCPU, mpfe_task_mode));

  auto mpfe_result = nll_mpfe->getVal();

  // create new nll without MPFE for creating nll_mp (an MPFE-enabled RooNLLVar interferes with MP::Vector's bipe use)
  auto nll = pdf->createNLL(*data);
  RooFit::MultiProcess::NLLVar nll_mp(NumCPU, mp_task_mode, *dynamic_cast<RooNLLVar*>(nll));

  auto mp_result = nll_mp.getVal();

  EXPECT_EQ(Hex(mpfe_result), Hex(mp_result));
  if (HasFailure()) {
    std::cout << "failed test had parameters NumCPU = " << NumCPU << ", task_mode = " << mp_task_mode << ", seed = " << seed << std::endl;
  }
}


TEST_P(NLLMultiProcessVsMPFE, minimize) {
  // do a minimization (e.g. like in GradMinimizer_Gaussian1D test)

  // TODO: see whether it performs adequately

  // parameters
  std::size_t NumCPU = std::get<0>(GetParam());
  RooFit::MultiProcess::NLLVarTask mp_task_mode = std::get<1>(GetParam());
  std::size_t seed = std::get<2>(GetParam());

  RooRandom::randomGenerator()->SetSeed(seed);

  RooWorkspace w = RooWorkspace();

  w.factory("Gaussian::g(x[-5,5],mu[0,-3,3],sigma[1])");
  auto x = w.var("x");
  RooAbsPdf *pdf = w.pdf("g");
  RooRealVar *mu = w.var("mu");

  RooDataSet *data = pdf->generate(RooArgSet(*x), 10000);
  mu->setVal(-2.9);

  int mpfe_task_mode;
  switch (mp_task_mode) {
    case RooFit::MultiProcess::NLLVarTask::bulk_partition: {
      mpfe_task_mode = 0;
      break;
    }
    case RooFit::MultiProcess::NLLVarTask::interleave: {
      mpfe_task_mode = 1;
      break;
    }
    default: {
      throw std::logic_error("can only compare bulk_partition and interleave strategies to MPFE NLL");
    }
  }

  auto nll_mpfe = pdf->createNLL(*data, RooFit::NumCPU(NumCPU, mpfe_task_mode));
  auto nll_nominal = pdf->createNLL(*data);
  RooFit::MultiProcess::NLLVar nll_mp(NumCPU, mp_task_mode, *dynamic_cast<RooNLLVar*>(nll_nominal));

  // save initial values for the start of all minimizations
  RooArgSet values = RooArgSet(*mu, *pdf);

  RooArgSet *savedValues = dynamic_cast<RooArgSet *>(values.snapshot());
  if (savedValues == nullptr) {
    throw std::runtime_error("params->snapshot() cannot be casted to RooArgSet!");
  }

  // --------

  RooMinimizer m0(*nll_mpfe);
  m0.setMinimizerType("Minuit2");

  m0.setStrategy(0);
  m0.setPrintLevel(-1);

  m0.migrad();

  RooFitResult *m0result = m0.lastMinuitFit();
  double minNll0 = m0result->minNll();
  double edm0 = m0result->edm();
  double mu0 = mu->getVal();
  double muerr0 = mu->getError();

  values = *savedValues;

  RooMinimizer m1(nll_mp);
  m1.setMinimizerType("Minuit2");

  m1.setStrategy(0);
  m1.setPrintLevel(-1);

  m1.migrad();

  RooFitResult *m1result = m1.lastMinuitFit();
  double minNll1 = m1result->minNll();
  double edm1 = m1result->edm();
  double mu1 = mu->getVal();
  double muerr1 = mu->getError();

  EXPECT_EQ(minNll0, minNll1);
  EXPECT_EQ(mu0, mu1);
  EXPECT_EQ(muerr0, muerr1);
  EXPECT_EQ(edm0, edm1);
}


INSTANTIATE_TEST_CASE_P(NworkersModeSeed,
                        NLLMultiProcessVsMPFE,
                        ::testing::Combine(::testing::Values(2,3,4),  // number of workers
                                           ::testing::Values(RooFit::MultiProcess::NLLVarTask::bulk_partition,
                                                             RooFit::MultiProcess::NLLVarTask::interleave),
                                           ::testing::Values(2,3)));  // random seed


TEST(NLLMultiProcessVsMPFE, throwOnCreatingMPwithMPFE) {
  // Using an MPFE-enabled NLL should throw when creating an MP NLL.
  RooWorkspace w;
  w.factory("Gaussian::g(x[-5,5],mu[0,-3,3],sigma[1])");
  auto x = w.var("x");
  RooAbsPdf *pdf = w.pdf("g");
  RooDataSet *data = pdf->generate(RooArgSet(*x), 10);

  auto nll_mpfe = pdf->createNLL(*data, RooFit::NumCPU(2));

  EXPECT_THROW({
    RooFit::MultiProcess::NLLVar nll_mp(2, RooFit::MultiProcess::NLLVarTask::bulk_partition, *dynamic_cast<RooNLLVar*>(nll_mpfe));
  }, std::logic_error);
}


class MultiProcessVsNominal : public ::testing::TestWithParam<std::tuple<std::size_t, std::size_t>> {};

TEST_P(MultiProcessVsNominal, GradMinimizer) {
  // do a minimization, but now using GradMinimizer and its MP version

  // parameters
  std::size_t NWorkers = std::get<0>(GetParam());
//  RooFit::MultiProcess::NLLVarTask mp_task_mode = std::get<1>(GetParam());
  std::size_t seed = std::get<1>(GetParam());

  RooRandom::randomGenerator()->SetSeed(seed);

  RooWorkspace w = RooWorkspace();

  w.factory("Gaussian::g(x[-5,5],mu[0,-3,3],sigma[1])");
  auto x = w.var("x");
  RooAbsPdf *pdf = w.pdf("g");
  RooRealVar *mu = w.var("mu");

  RooDataSet *data = pdf->generate(RooArgSet(*x), 10000);
  mu->setVal(-2.9);

  auto nll = pdf->createNLL(*data);

  // save initial values for the start of all minimizations
  RooArgSet values = RooArgSet(*mu, *pdf, *nll);

  RooArgSet *savedValues = dynamic_cast<RooArgSet *>(values.snapshot());
  if (savedValues == nullptr) {
    throw std::runtime_error("params->snapshot() cannot be casted to RooArgSet!");
  }

  // --------

  RooGradMinimizer m0(*nll);
  m0.setMinimizerType("Minuit2");

  m0.setStrategy(0);
  m0.setPrintLevel(-1);

  m0.migrad();

  RooFitResult *m0result = m0.lastMinuitFit();
  double minNll0 = m0result->minNll();
  double edm0 = m0result->edm();
  double mu0 = mu->getVal();
  double muerr0 = mu->getError();

  values = *savedValues;

  RooFit::MultiProcess::GradMinimizer m1(*nll);
  m1.setMinimizerType("Minuit2");

  m1.setStrategy(0);
  m1.setPrintLevel(-1);

  m1.migrad();

  RooFitResult *m1result = m1.lastMinuitFit();
  double minNll1 = m1result->minNll();
  double edm1 = m1result->edm();
  double mu1 = mu->getVal();
  double muerr1 = mu->getError();

  EXPECT_EQ(minNll0, minNll1);
  EXPECT_EQ(mu0, mu1);
  EXPECT_EQ(muerr0, muerr1);
  EXPECT_EQ(edm0, edm1);
}


INSTANTIATE_TEST_CASE_P(NworkersSeed,
                        MultiProcessVsNominal,
                        ::testing::Combine(::testing::Values(1,2,3),  // number of workers
                                           ::testing::Values(2,3)));  // random seed
