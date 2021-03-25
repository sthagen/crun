/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017, 2018, 2019 Giuseppe Scrivano <giuseppe@scrivano.org>
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "crun.h"
#include "libcrun/container.h"
#include "libcrun/utils.h"

static char doc[] = "OCI runtime";

static char *resources = NULL;

static libcrun_context_t crun_context;

enum
{
  FIRST_VALUE = 1000,

  BLKIO_WEIGHT = FIRST_VALUE,

  CPU_PERIOD,
  CPU_QUOTA,
  CPU_SHARE,
  CPU_RT_PERIOD,
  CPU_RT_RUNTIME,
  CPUSET_CPUS,
  CPUSET_MEMS,

  KERNEL_MEMORY,
  KERNEL_MEMORY_TCP,
  MEMORY,
  MEMORY_RESERVATION,
  MEMORY_SWAP,

  PIDS_LIMIT,

  LAST_VALUE,
};

struct description_s
{
  int id;
  unsigned int section;
  const char *key;
  int numeric;
};

static const char *sections[] = { "blockIO", "cpu", "memory", "pids" };

static struct description_s descriptors[] = { { BLKIO_WEIGHT, 0, "weight", 1 },

                                              { CPU_PERIOD, 1, "period", 1 },
                                              { CPU_QUOTA, 1, "quota", 1 },
                                              { CPU_SHARE, 1, "share", 1 },
                                              { CPU_RT_PERIOD, 1, "realtimePeriod", 1 },
                                              { CPU_RT_RUNTIME, 1, "realtimeRuntime", 1 },
                                              { CPUSET_CPUS, 1, "cpus", 0 },
                                              { CPUSET_MEMS, 1, "mems", 0 },

                                              { KERNEL_MEMORY, 2, "kernel", 1 },
                                              { KERNEL_MEMORY_TCP, 2, "kernelTCP", 1 },
                                              { MEMORY, 2, "limit", 1 },
                                              { MEMORY_RESERVATION, 2, "reservation", 1 },
                                              { MEMORY_SWAP, 2, "swap", 1 },

                                              { PIDS_LIMIT, 3, "limit", 1 },
                                              { 0 } };

static const char *values[LAST_VALUE - FIRST_VALUE];

static void
set_value (int id, const char *value)
{
  values[id - FIRST_VALUE] = value;
}

static struct argp_option options[]
    = { { "resources", 'r', "FILE", 0, "path to the file containing the resources to update", 0 },
        { "blkio-weight", BLKIO_WEIGHT, "VALUE", 0, "Specifies per cgroup weight", 0 },
        { "cpu-period", CPU_PERIOD, "VALUE", 0, "CPU CFS period to be used for hardcapping", 0 },
        { "cpu-quota", CPU_QUOTA, "VALUE", 0, "CPU CFS hardcap limit", 0 },
        { "cpu-share", CPU_SHARE, "VALUE", 0, "CPU shares", 0 },
        { "cpu-rt-period", CPU_RT_PERIOD, "VALUE", 0, "CPU realtime period to be used for hardcapping", 0 },
        { "cpu-rt-runtime", CPU_RT_RUNTIME, "VALUE", 0, "CPU realtime hardcap limit", 0 },
        { "cpuset-cpus", CPUSET_CPUS, "VALUE", 0, "CPU(s) to use", 0 },
        { "cpuset-mems", CPUSET_MEMS, "VALUE", 0, "Memory node(s) to use", 0 },
        { "kernel-memory", KERNEL_MEMORY, "VALUE", 0, "Kernel memory limit", 0 },
        { "kernel-memory-tcp", KERNEL_MEMORY_TCP, "VALUE", 0, "Kernel memory limit for tcp buffer", 0 },
        { "memory", MEMORY, "VALUE", 0, "Memory limit", 0 },
        { "memory-reservation", MEMORY_RESERVATION, "VALUE", 0, "Memory reservation or soft_limit", 0 },
        { "memory-swap", MEMORY_SWAP, "VALUE", 0, "Total memory usage", 0 },
        { "pids-limit", PIDS_LIMIT, "VALUE", 0, "Maximum number of pids allowed in the container", 0 },
        {
            0,
        } };

#define YAJL_STR(x) ((const unsigned char *) (x))

static const unsigned char *
build_file (size_t *len)
{
  size_t i;
  yajl_gen gen = NULL;
  size_t n_sections = sizeof (sections) / sizeof (sections[0]);
  int has_sections[n_sections];
  const unsigned char *buf;

  memset (has_sections, 0, sizeof (has_sections));

  gen = yajl_gen_alloc (NULL);
  if (gen == NULL)
    error (EXIT_FAILURE, errno, "yajl_gen_alloc failed");

  for (i = 0; i < LAST_VALUE - FIRST_VALUE; i++)
    if (values[i])
      has_sections[descriptors[i].section] = 1;

  for (i = 0; i < n_sections; i++)
    {
      if (values[i])
        has_sections[descriptors[i].section] = 1;
    }

  yajl_gen_config (gen, yajl_gen_beautify, 1);
  yajl_gen_config (gen, yajl_gen_validate_utf8, 1);

  yajl_gen_map_open (gen);
  for (i = 0; i < n_sections; i++)
    {
      size_t j;

      if (! has_sections[i])
        continue;

      yajl_gen_string (gen, YAJL_STR (sections[i]), strlen (sections[i]));
      yajl_gen_map_open (gen);

      for (j = 0; j < LAST_VALUE - FIRST_VALUE; j++)
        {
          struct description_s *d = &descriptors[j];
          if (values[j] == NULL || d->section != i)
            continue;

          yajl_gen_string (gen, YAJL_STR (d->key), strlen (d->key));
          if (! d->numeric)
            yajl_gen_string (gen, YAJL_STR (values[j]), strlen (values[j]));
          else
            {
              long value;
              errno = 0;
              value = strtoul (values[j], NULL, 10);
              if (errno != 0)
                error (EXIT_FAILURE, errno, "invalid setting");

              yajl_gen_integer (gen, value);
            }
        }

      yajl_gen_map_close (gen);
    }
  yajl_gen_map_close (gen);

  yajl_gen_get_buf (gen, &buf, len);
  return buf;
}

static char args_doc[] = "update [OPTION]... CONTAINER";

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'r':
      resources = argp_mandatory_argument (arg, state);
      break;

    case ARGP_KEY_NO_ARGS:
      libcrun_fail_with_error (0, "please specify a ID for the container");

    case BLKIO_WEIGHT:
    case CPU_PERIOD:
    case CPU_QUOTA:
    case CPU_SHARE:
    case CPU_RT_PERIOD:
    case CPU_RT_RUNTIME:
    case CPUSET_CPUS:
    case CPUSET_MEMS:
    case KERNEL_MEMORY:
    case KERNEL_MEMORY_TCP:
    case MEMORY:
    case MEMORY_RESERVATION:
    case MEMORY_SWAP:
    case PIDS_LIMIT:
      set_value (key, argp_mandatory_argument (arg, state));
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }

  return 0;
}

static struct argp run_argp = { options, parse_opt, args_doc, doc, NULL, NULL, NULL };

int
crun_command_update (struct crun_global_arguments *global_args, int argc, char **argv, libcrun_error_t *err)
{
  int first_arg = 0, ret;
  char *content = NULL;
  size_t len;

  argp_parse (&run_argp, argc, argv, ARGP_IN_ORDER, &first_arg, &crun_context);
  crun_assert_n_args (argc - first_arg, 1, 1);

  ret = init_libcrun_context (&crun_context, argv[first_arg], global_args, err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (resources == NULL)
    {
      content = (char *) build_file (&len);
      return libcrun_container_update (&crun_context, argv[first_arg], content, len, err);
    }

  return libcrun_container_update_from_file (&crun_context, argv[first_arg], resources, err);
}
