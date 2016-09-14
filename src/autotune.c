/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#include "common.h"
#include "types_int.h"
#include "types.h"
#include "logging.h"
#include "interface.h"
#include "timer.h"
#include "ext_OpenCL.h"
#include "ext_ADL.h"
#include "ext_nvapi.h"
#include "ext_nvml.h"
#include "ext_xnvctrl.h"
#include "hwmon.h"
#include "mpsp.h"
#include "rp_cpu.h"
#include "restore.h"
#include "opencl.h"
#include "outfile.h"
#include "potfile.h"
#include "debugfile.h"
#include "loopback.h"
#include "data.h"
#include "status.h"
#include "autotune.h"

extern hc_global_data_t data;

static double TARGET_MS_PROFILE[4] = { 2, 12, 96, 480 };

static double try_run (hc_device_param_t *device_param, hashconfig_t *hashconfig, const u32 kernel_accel, const u32 kernel_loops)
{
  const u32 kernel_power_try = device_param->device_processors * device_param->kernel_threads * kernel_accel;

  device_param->kernel_params_buf32[28] = 0;
  device_param->kernel_params_buf32[29] = kernel_loops; // not a bug, both need to be set
  device_param->kernel_params_buf32[30] = kernel_loops; // because there's two variables for inner iters for slow and fast hashes

  if (hashconfig->attack_exec == ATTACK_EXEC_INSIDE_KERNEL)
  {
    run_kernel (KERN_RUN_1, device_param, kernel_power_try, true, 0, hashconfig);
  }
  else
  {
    run_kernel (KERN_RUN_2, device_param, kernel_power_try, true, 0, hashconfig);
  }

  const double exec_ms_prev = get_avg_exec_time (device_param, 1);

  return exec_ms_prev;
}

int autotune (hc_device_param_t *device_param, hashconfig_t *hashconfig)
{
  const double target_ms = TARGET_MS_PROFILE[data.workload_profile - 1];

  const u32 kernel_accel_min = device_param->kernel_accel_min;
  const u32 kernel_accel_max = device_param->kernel_accel_max;

  const u32 kernel_loops_min = device_param->kernel_loops_min;
  const u32 kernel_loops_max = device_param->kernel_loops_max;

  u32 kernel_accel = kernel_accel_min;
  u32 kernel_loops = kernel_loops_min;

  // in this case the user specified a fixed -u and -n on the commandline
  // no way to tune anything
  // but we need to run a few caching rounds

  if ((kernel_loops_min == kernel_loops_max) && (kernel_accel_min == kernel_accel_max))
  {
    if (hashconfig->hash_mode != 2000)
    {
      try_run (device_param, hashconfig, kernel_accel, kernel_loops);
      try_run (device_param, hashconfig, kernel_accel, kernel_loops);
      try_run (device_param, hashconfig, kernel_accel, kernel_loops);
      try_run (device_param, hashconfig, kernel_accel, kernel_loops);
    }

    device_param->kernel_accel = kernel_accel;
    device_param->kernel_loops = kernel_loops;

    const u32 kernel_power = device_param->device_processors * device_param->kernel_threads * device_param->kernel_accel;

    device_param->kernel_power = kernel_power;

    return 0;
  }

  // from here it's clear we are allowed to autotune
  // so let's init some fake words

  const u32 kernel_power_max = device_param->device_processors * device_param->kernel_threads * kernel_accel_max;

  if (data.attack_kern == ATTACK_KERN_BF)
  {
    run_kernel_memset (device_param, device_param->d_pws_buf, 7, kernel_power_max * sizeof (pw_t));
  }
  else
  {
    for (u32 i = 0; i < kernel_power_max; i++)
    {
      device_param->pws_buf[i].i[0]   = i;
      device_param->pws_buf[i].i[1]   = 0x01234567;
      device_param->pws_buf[i].pw_len = 7 + (i & 7);
    }

    cl_int CL_err = hc_clEnqueueWriteBuffer (data.ocl, device_param->command_queue, device_param->d_pws_buf, CL_TRUE, 0, kernel_power_max * sizeof (pw_t), device_param->pws_buf, 0, NULL, NULL);

    if (CL_err != CL_SUCCESS)
    {
      log_error ("ERROR: clEnqueueWriteBuffer(): %s\n", val2cstr_cl (CL_err));

      return -1;
    }
  }

  if (hashconfig->attack_exec == ATTACK_EXEC_INSIDE_KERNEL)
  {
    if (data.kernel_rules_cnt > 1)
    {
      cl_int CL_err = hc_clEnqueueCopyBuffer (data.ocl, device_param->command_queue, device_param->d_rules, device_param->d_rules_c, 0, 0, MIN (kernel_loops_max, KERNEL_RULES) * sizeof (kernel_rule_t), 0, NULL, NULL);

      if (CL_err != CL_SUCCESS)
      {
        log_error ("ERROR: clEnqueueCopyBuffer(): %s\n", val2cstr_cl (CL_err));

        return -1;
      }
    }
  }
  else
  {
    run_kernel_amp (device_param, kernel_power_max);
  }

  #define VERIFIER_CNT 1

  // first find out highest kernel-loops that stays below target_ms

  if (kernel_loops_min < kernel_loops_max)
  {
    for (kernel_loops = kernel_loops_max; kernel_loops > kernel_loops_min; kernel_loops >>= 1)
    {
      double exec_ms = try_run (device_param, hashconfig, kernel_accel_min, kernel_loops);

      for (int i = 0; i < VERIFIER_CNT; i++)
      {
        double exec_ms_v = try_run (device_param, hashconfig, kernel_accel_min, kernel_loops);

        exec_ms = MIN (exec_ms, exec_ms_v);
      }

      if (exec_ms < target_ms) break;
    }
  }

  // now the same for kernel-accel but with the new kernel-loops from previous loop set

  #define STEPS_CNT 10

  if (kernel_accel_min < kernel_accel_max)
  {
    for (int i = 0; i < STEPS_CNT; i++)
    {
      const u32 kernel_accel_try = 1u << i;

      if (kernel_accel_try < kernel_accel_min) continue;
      if (kernel_accel_try > kernel_accel_max) break;

      double exec_ms = try_run (device_param, hashconfig, kernel_accel_try, kernel_loops);

      for (int i = 0; i < VERIFIER_CNT; i++)
      {
        double exec_ms_v = try_run (device_param, hashconfig, kernel_accel_try, kernel_loops);

        exec_ms = MIN (exec_ms, exec_ms_v);
      }

      if (exec_ms > target_ms) break;

      kernel_accel = kernel_accel_try;
    }
  }

  // at this point we want to know the actual runtime for the following reason:
  // we need a reference for the balancing loop following up, and this
  // the balancing loop can have an effect that the creates a new opportunity, for example:
  //   if the target is 95 ms and the current runtime is 48ms the above loop
  //   stopped the execution because the previous exec_ms was > 95ms
  //   due to the rebalance it's possible that the runtime reduces from 48ms to 47ms
  //   and this creates the possibility to double the workload -> 47 * 2 = 95ms, which is < 96ms

  double exec_ms_pre_final = try_run (device_param, hashconfig, kernel_accel, kernel_loops);

  for (int i = 0; i < VERIFIER_CNT; i++)
  {
    double exec_ms_pre_final_v = try_run (device_param, hashconfig, kernel_accel, kernel_loops);

    exec_ms_pre_final = MIN (exec_ms_pre_final, exec_ms_pre_final_v);
  }

  u32 diff = kernel_loops - kernel_accel;

  if ((kernel_loops_min < kernel_loops_max) && (kernel_accel_min < kernel_accel_max))
  {
    u32 kernel_accel_orig = kernel_accel;
    u32 kernel_loops_orig = kernel_loops;

    for (u32 f = 1; f < 1024; f++)
    {
      const u32 kernel_accel_try = kernel_accel_orig * f;
      const u32 kernel_loops_try = kernel_loops_orig / f;

      if (kernel_accel_try > kernel_accel_max) break;
      if (kernel_loops_try < kernel_loops_min) break;

      u32 diff_new = kernel_loops_try - kernel_accel_try;

      if (diff_new > diff) break;

      diff_new = diff;

      double exec_ms = try_run (device_param, hashconfig, kernel_accel_try, kernel_loops_try);

      for (int i = 0; i < VERIFIER_CNT; i++)
      {
        double exec_ms_v = try_run (device_param, hashconfig, kernel_accel_try, kernel_loops_try);

        exec_ms = MIN (exec_ms, exec_ms_v);
      }

      if (exec_ms < exec_ms_pre_final)
      {
        exec_ms_pre_final = exec_ms;

        kernel_accel = kernel_accel_try;
        kernel_loops = kernel_loops_try;
      }
    }
  }

  const double exec_left = target_ms / exec_ms_pre_final;

  const double accel_left = kernel_accel_max / kernel_accel;

  const double exec_accel_min = MIN (exec_left, accel_left); // we want that to be int

  if (exec_accel_min >= 1.0)
  {
    // this is safe to not overflow kernel_accel_max because of accel_left

    kernel_accel *= (u32) exec_accel_min;
  }

  // reset them fake words

  /*
  memset (device_param->pws_buf, 0, kernel_power_max * sizeof (pw_t));

  hc_clEnqueueWriteBuffer (data.ocl, device_param->command_queue, device_param->d_pws_buf,     CL_TRUE, 0, kernel_power_max * sizeof (pw_t), device_param->pws_buf, 0, NULL, NULL);
  hc_clEnqueueWriteBuffer (data.ocl, device_param->command_queue, device_param->d_pws_amp_buf, CL_TRUE, 0, kernel_power_max * sizeof (pw_t), device_param->pws_buf, 0, NULL, NULL);
  */

  run_kernel_memset (device_param, device_param->d_pws_buf, 0, kernel_power_max * sizeof (pw_t));

  if (hashconfig->attack_exec == ATTACK_EXEC_OUTSIDE_KERNEL)
  {
    run_kernel_memset (device_param, device_param->d_pws_amp_buf, 0, kernel_power_max * sizeof (pw_t));
  }

  // reset timer

  device_param->exec_pos = 0;

  memset (device_param->exec_ms, 0, EXEC_CACHE * sizeof (double));

  memset (device_param->exec_us_prev1, 0, EXPECTED_ITERATIONS * sizeof (double));
  memset (device_param->exec_us_prev2, 0, EXPECTED_ITERATIONS * sizeof (double));
  memset (device_param->exec_us_prev3, 0, EXPECTED_ITERATIONS * sizeof (double));

  // store

  device_param->kernel_accel = kernel_accel;
  device_param->kernel_loops = kernel_loops;

  const u32 kernel_power = device_param->device_processors * device_param->kernel_threads * device_param->kernel_accel;

  device_param->kernel_power = kernel_power;

  #if defined (DEBUG)

  if (data.quiet == 0)
  {
    clear_prompt ();

    log_info ("- Device #%u: autotuned kernel-accel to %u\n"
              "- Device #%u: autotuned kernel-loops to %u\n",
              device_param->device_id + 1, kernel_accel,
              device_param->device_id + 1, kernel_loops);

    send_prompt ();
  }

  #endif

  return 0;
}