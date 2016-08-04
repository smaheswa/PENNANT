/*
 * Parallel.hh
 *
 *  Created on: May 31, 2013
 *      Author: cferenba
 *
 * Copyright (c) 2013, Los Alamos National Security, LLC.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style open-source
 * license; see top-level LICENSE file for full license text.
 */

#ifndef PARALLEL_HH_
#define PARALLEL_HH_

#include <stdint.h>

#include "InputParameters.hh"

#include "legion.h"
using namespace LegionRuntime::HighLevel;
using namespace LegionRuntime::Accessor;

// Namespace Parallel provides helper functions and variables for
// running in distributed parallel mode using MPI, or for stubbing
// these out if not using MPI.

// JPG TODO: make this an actual object

namespace Parallel {
    extern int num_subregions;           // number of MPI PEs in use
                                // (1 if not using MPI)
    extern int mype;            // PE number for my rank
                                // (0 if not using MPI)
	extern MustEpochLauncher must_epoch_launcher;
	extern Context ctx_;
	extern HighLevelRuntime *runtime_;

    void init(InputParameters input_params,
    		Context ctx, HighLevelRuntime *runtime);
    void run();
    void finalize();

    void globalMinLoc(double& x, int& xpe);
                                // find minimum over all PEs, and
                                // report which PE had the minimum
    void globalSum(int& x);     // find sum over all PEs - overloaded
    void globalSum(int64_t& x);
    void globalSum(double& x);
    void gather(const int x, int* y);
                                // gather list of ints from all PEs
    void scatter(const int* x, int& y);
                                // gather list of ints from all PEs

    template<typename T>
    void gatherv(               // gather variable-length list
            const T *x, const int numx,
            T* y, const int* numy);
    template<typename T>
    void gathervImpl(           // helper function for gatherv
            const T *x, const int numx,
            T* y, const int* numy);

}  // namespace Parallel


struct SPMDArgs {
    InputParameters input_params_;
	DynamicCollective add_reduction_;
	DynamicCollective min_reduction_;
	int shard_id_;
};

struct TimeStep {
	double dt_;
	char message_[80];
	TimeStep() {
		dt_ = std::numeric_limits<double>::max();
		snprintf(message_, 80, "Error: uninitialized");
	}
	TimeStep(const TimeStep &copy) {
		dt_ = copy.dt_;
		snprintf(message_, 80, "%s", copy.message_);
	}
	inline friend bool operator<(const TimeStep &l, const TimeStep &r) {
		return l.dt_ < r.dt_;
	}
	inline friend bool operator>(const TimeStep &l, const TimeStep &r) {
		return r < l;
	}
	inline friend bool operator<=(const TimeStep &l, const TimeStep &r) {
		return !(l > r);
	}
	inline friend bool operator>=(const TimeStep &l, const TimeStep &r) {
		return !(l < r);
	}
	inline friend bool operator==(const TimeStep &l, const TimeStep &r) {
		return l.dt_ == r.dt_;
	}
	inline friend bool operator!=(const TimeStep &l, const TimeStep &r) {
		return !(l == r);
	}
};

enum TaskIDs {
	TOP_LEVEL_TASK_ID,
	DRIVER_TASK_ID,
	GLOBAL_SUM_TASK_ID,
	GLOBAL_MIN_TASK_ID,
	ADD_REDOP_ID,
	MIN_REDOP_ID,
};

enum Variants {
  CPU_VARIANT,
  GPU_VARIANT,
};

namespace TaskHelper {
  template<typename T>
  void base_cpu_wrapper(const Task *task,
                        const std::vector<PhysicalRegion> &regions,
                        Context ctx, HighLevelRuntime *runtime)
  {
    T::cpu_run(task, regions, ctx, runtime);
  }

#ifdef USE_CUDA
  template<typename T>
  void base_gpu_wrapper(const Task *task,
                        const std::vector<PhysicalRegion> &regions,
                        Context ctx, HighLevelRuntime *runtime)
  {
	const int *p = (int*)task->local_args;
    T::gpu_run(*p, regions);
  }
#endif

  template<typename T>
  void register_cpu_variants(void)
  {
    HighLevelRuntime::register_legion_task<base_cpu_wrapper<T> >(T::TASK_ID, Processor::LOC_PROC,
                                                                 false/*single*/, true/*index*/,
                                                                 CPU_VARIANT,
                                                                 TaskConfigOptions(T::CPU_BASE_LEAF),
                                                                 T::TASK_NAME);
  }

};

#endif /* PARALLEL_HH_ */
