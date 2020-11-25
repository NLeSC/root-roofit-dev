/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 * @(#)root/roofitcore:$Id$
 * Authors:                                                                  *
 *   WV, Wouter Verkerke, UC Santa Barbara, verkerke@slac.stanford.edu       *
 *   DK, David Kirkby,    UC Irvine,         dkirkby@uci.edu                 *
 *   PB, Patrick Bos,     NL eScience Center, p.bos@esciencecenter.nl        *
 *                                                                           *
 * Copyright (c) 2000-2020, Regents of the University of California          *
 *                          and Stanford University. All rights reserved.    *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/

/**
\file RooUnbinnedL.cxx
\class RooUnbinnedL
\ingroup Roofitcore

Class RooUnbinnedL implements a a -log(likelihood) calculation from a dataset
and a PDF. The NLL is calculated as
<pre>
 Sum[data] -log( pdf(x_data) )
</pre>
In extended mode, a (Nexpect - Nobserved*log(NExpected) term is added
**/

#include "RooAbsData.h"
#include "RooAbsPdf.h"
#include "RooAbsDataStore.h"

#include <TestStatistics/RooUnbinnedL.h>

namespace RooFit {
namespace TestStatistics {

RooUnbinnedL::RooUnbinnedL(RooAbsPdf *pdf, RooAbsData *data, bool do_offset, double offset, double offset_carry,
                           RooAbsL::Extended extended)
   : RooAbsL(pdf, data, do_offset, offset, offset_carry, data->numEntries(), 1, extended)
{}

RooUnbinnedL::RooUnbinnedL(const RooUnbinnedL &other)
   : RooAbsL(other), apply_weight_squared(other.apply_weight_squared), _first(other._first),
     _offset_save_weight_squared(other._offset_save_weight_squared),
     _offset_carry_save_weight_squared(other._offset_carry_save_weight_squared), _evalCarry(other._evalCarry)
{}


bool RooUnbinnedL::processEmptyDataSets() const
{
   return extended_;
}

//////////////////////////////////////////////////////////////////////////////////

void RooUnbinnedL::set_apply_weight_squared(bool flag)
{
   if (flag != apply_weight_squared) {
      apply_weight_squared = flag;
      std::swap(_offset, _offset_save_weight_squared);
      std::swap(_offset_carry, _offset_carry_save_weight_squared);
   }
   //   setValueDirty();
}

//////////////////////////////////////////////////////////////////////////////////
///// Calculate and return likelihood on subset of data from firstEvent to lastEvent
///// processed with a step size of 'stepSize'. If this an extended likelihood and
///// and the zero event is processed the extended term is added to the return
///// likelihood.
//
double RooUnbinnedL::evaluate_partition(std::size_t events_begin, std::size_t events_end,
                                        std::size_t /*components_begin*/, std::size_t /*components_end*/)
{
   // Throughout the calculation, we use Kahan's algorithm for summing to
   // prevent loss of precision - this is a factor four more expensive than
   // straight addition, but since evaluating the PDF is usually much more
   // expensive than that, we tolerate the additional cost...
   Double_t result(0), carry(0);

   // cout << "RooNLLVar::evaluatePartition(" << GetName() << ") projDeps = " << (_projDeps?*_projDeps:RooArgSet()) <<
   // endl ;

   //   data->store()->recalculateCache(_projDeps, firstEvent, lastEvent, stepSize, (_binnedPdf?kFALSE:kTRUE));
   // TODO: check when we might need _projDeps (it seems to be mostly empty); ties in with TODO below
   data->store()->recalculateCache(nullptr, events_begin, events_end, 1, kTRUE);

   Double_t sumWeight(0), sumWeightCarry(0);

   for (std::size_t i = events_begin; i < events_end; ++i) {
      data->get(i);
      if (!data->valid()) {
         continue;
      }

      Double_t eventWeight = data->weight();
      if (0. == eventWeight * eventWeight) {
         continue;
      }
      if (apply_weight_squared) {
         eventWeight = data->weightSquared();
      }

      Double_t term = -eventWeight * pdf->getLogVal(_normSet);
      // TODO: _normSet should be modified if _projDeps is non-null, connected to TODO above

      Double_t y = eventWeight - sumWeightCarry;
      Double_t t = sumWeight + y;
      sumWeightCarry = (t - sumWeight) - y;
      sumWeight = t;

      y = term - carry;
      t = result + y;
      carry = (t - result) - y;
      result = t;
   }

   // include the extended maximum likelihood term, if requested
   if (extended_) {
      if (apply_weight_squared) {

         // Calculate sum of weights-squared here for extended term
         Double_t sumW2(0), sumW2carry(0);
         for (std::size_t i = 0; i < data->numEntries(); i++) {
            data->get(i);
            Double_t y = data->weightSquared() - sumW2carry;
            Double_t t = sumW2 + y;
            sumW2carry = (t - sumW2) - y;
            sumW2 = t;
         }

         Double_t expected = pdf->expectedEvents(data->get());

         // Adjust calculation of extended term with W^2 weighting: adjust poisson such that
         // estimate of Nexpected stays at the same value, but has a different variance, rescale
         // both the observed and expected count of the Poisson with a factor sum[w] / sum[w^2] which is
         // the effective weight of the Poisson term.
         // i.e. change Poisson(Nobs = sum[w]| Nexp ) --> Poisson( sum[w] * sum[w] / sum[w^2] | Nexp * sum[w] /
         // sum[w^2] ) weighted by the effective weight  sum[w^2]/ sum[w] in the likelihood. Since here we compute
         // the likelihood with the weight square we need to multiply by the square of the effective weight expectedW
         // = expected * sum[w] / sum[w^2]   : effective expected entries observedW =  sum[w]  * sum[w] / sum[w^2] :
         // effective observed entries The extended term for the likelihood weighted by the square of the weight will
         // be then:
         //  (sum[w^2]/ sum[w] )^2 * expectedW -  (sum[w^2]/ sum[w] )^2 * observedW * log (expectedW)  and this is
         //  using the previous expressions for expectedW and observedW
         //  sum[w^2] / sum[w] * expected - sum[w^2] * log (expectedW)
         //  and since the weights are constants in the likelihood we can use log(expected) instead of log(expectedW)

         Double_t expectedW2 = expected * sumW2 / data->sumEntries();
         Double_t extra = expectedW2 - sumW2 * log(expected);

         // Double_t y = pdf->extendedTerm(sumW2, data->get()) - carry;

         Double_t y = extra - carry;

         Double_t t = result + y;
         carry = (t - result) - y;
         result = t;
      } else {
         Double_t y = pdf->extendedTerm(data->sumEntries(), data->get()) - carry;
         Double_t t = result + y;
         carry = (t - result) - y;
         result = t;
      }
   }

   // TODO: check if this can indeed be left out for (un)binned likelihoods
   //   // If part of simultaneous PDF normalize probability over
   //   // number of simultaneous PDFs: -sum(log(p/n)) = -sum(log(p)) + N*log(n)
   //   if (_simCount > 1) {
   //      Double_t y = sumWeight * log(1.0 * _simCount) - carry;
   //      Double_t t = result + y;
   //      carry = (t - result) - y;
   //      result = t;
   //   }

   // timer.Stop() ;
   // cout << "RooNLLVar::evalPart(" << GetName() << ") SET=" << _setNum << " first=" << firstEvent << ", last=" <<
   // lastEvent << ", step=" << stepSize << ") result = " << result << " CPU = " << timer.CpuTime() << endl ;

   // At the end of the first full calculation, wire the caches
   if (_first) {
      _first = false;
      pdf->wireAllCaches();
   }

   // Check if value offset flag is set.
   if (_do_offset) {

      // If no offset is stored enable this feature now
      if (_offset == 0 && result != 0) {
         oocoutI(static_cast<RooAbsArg *>(nullptr), Minimization)
            << "RooUnbinnedL::evaluate_partition(" << GetName() << ") first = " << events_begin
            << " last = " << events_end << " Likelihood offset now set to " << result << std::endl;
         _offset = result;
         _offset_carry = carry;
      }

      // Substract offset
      Double_t y = -_offset - (carry + _offset_carry);
      Double_t t = result + y;
      carry = (t - result) - y;
      result = t;
   }

   _evalCarry = carry;
   return result;
}

double RooUnbinnedL::get_carry() const
{
   return _evalCarry;
}

} // namespace TestStatistics
} // namespace RooFit
