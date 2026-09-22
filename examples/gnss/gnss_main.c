/****************************************************************************
 * apps/examples/gnss/gnss_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uORB/uORB.h>
#include <sensor/gnss.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GNSS_NCONSTELLATIONS   7    /* SENSOR_GNSS_CONSTELLATION_* */
#define GNSS_STATUS_INTERVAL   10   /* Seconds between waiting reports */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char *const g_constellations[GNSS_NCONSTELLATIONS] =
{
  "?", "GPS", "SBAS", "GLONASS", "QZSS", "BeiDou", "Galileo"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void gnss_usage(FAR const char *progname)
{
  printf("Usage: %s [-t seconds] [-n fixes] [-s]\n"
         "\n"
         "Power the GNSS receiver and print\n"
         "the position once it has a fix.\n"
         "  -t <s>  give up after s seconds\n"
         "          (default 300)\n"
         "  -n <n>  print n fixes, one a second\n"
         "          (default 1, 0 = until -t)\n"
         "  -s      set the system clock from\n"
         "          the first fix\n",
         progname);
}

static uint32_t gnss_seconds_since(FAR const struct timespec *start)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec - start->tv_sec;
}

static void gnss_print_waiting(uint32_t elapsed,
                               FAR const uint32_t *inview)
{
  uint32_t total = 0;
  int i;

  for (i = 0; i < GNSS_NCONSTELLATIONS; i++)
    {
      total += inview[i];
    }

  printf("%3" PRIu32 " s: %" PRIu32 " satellites in view\n", elapsed, total);

  for (i = 0; i < GNSS_NCONSTELLATIONS; i++)
    {
      if (inview[i] > 0)
        {
          printf("       %-8s %" PRIu32 "\n", g_constellations[i],
                 inview[i]);
        }
    }
}

static void gnss_print_fix(FAR const struct sensor_gnss *fix)
{
  struct tm tm;
  time_t utc = (time_t)fix->time_utc;

  printf("  %.6f %c  %.6f %c\n",
         fabsf(fix->latitude), fix->latitude < 0 ? 'S' : 'N',
         fabsf(fix->longitude), fix->longitude < 0 ? 'W' : 'E');

  printf("  altitude %.0f m, %" PRIu32 " satellites\n",
         fix->altitude, fix->satellites_used);

  if (!isnan(fix->hdop))
    {
      printf("  HDOP %.1f", fix->hdop);
      if (!isnan(fix->ground_speed))
        {
          printf(", speed %.1f m/s", fix->ground_speed);
        }

      printf("\n");
    }

  if (gmtime_r(&utc, &tm) != NULL)
    {
      printf("  %04d-%02d-%02d %02d:%02d:%02d UTC\n",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

static void gnss_set_clock(FAR const struct sensor_gnss *fix)
{
  struct timespec ts;

  ts.tv_sec  = (time_t)fix->time_utc;
  ts.tv_nsec = 0;

  if (clock_settime(CLOCK_REALTIME, &ts) < 0)
    {
      fprintf(stderr, "ERROR: clock_settime failed: %d\n", errno);
    }
  else
    {
      printf("System clock set\n");
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint32_t inview[GNSS_NCONSTELLATIONS];
  struct sensor_gnss_satellite satellite;
  struct sensor_gnss fix;
  struct pollfd fds[2];
  struct timespec start;
  uint32_t lastreport = 0;
  uint32_t elapsed = 0;
  bool settime = false;
  int timeout = 300;
  int count = 1;
  int fixes = 0;
  int ch;

  while ((ch = getopt(argc, argv, "t:n:sh")) != ERROR)
    {
      switch (ch)
        {
          case 't':
            timeout = atoi(optarg);
            break;

          case 'n':
            count = atoi(optarg);
            break;

          case 's':
            settime = true;
            break;

          default:
            gnss_usage(argv[0]);
            return ch == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

  if (timeout < 1 || count < 0)
    {
      gnss_usage(argv[0]);
      return EXIT_FAILURE;
    }

  /* Subscribing powers the receiver; unsubscribing lets it switch off */

  fds[0].fd = orb_subscribe(ORB_ID(sensor_gnss));
  fds[1].fd = orb_subscribe(ORB_ID(sensor_gnss_satellite));
  if (fds[0].fd < 0 || fds[1].fd < 0)
    {
      fprintf(stderr, "ERROR: no GNSS receiver: %d\n", errno);
      goto out;
    }

  memset(inview, 0, sizeof(inview));
  printf("Waiting up to %d s for a fix\n", timeout);
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (elapsed < (uint32_t)timeout && (count == 0 || fixes < count))
    {
      fds[0].events = POLLIN;
      fds[1].events = POLLIN;

      if (poll(fds, 2, 1000) > 0)
        {
          if ((fds[1].revents & POLLIN) != 0 &&
              orb_copy(ORB_ID(sensor_gnss_satellite), fds[1].fd,
                       &satellite) == OK &&
              satellite.constellation < GNSS_NCONSTELLATIONS)
            {
              inview[satellite.constellation] = satellite.satellites;
            }

          /* Positions are only published once there is a fix */

          if ((fds[0].revents & POLLIN) != 0 &&
              orb_copy(ORB_ID(sensor_gnss), fds[0].fd, &fix) == OK &&
              !isnan(fix.latitude))
            {
              if (fixes++ == 0)
                {
                  printf("Fix after %" PRIu32 " s\n",
                         gnss_seconds_since(&start));
                  if (settime)
                    {
                      gnss_set_clock(&fix);
                    }
                }

              gnss_print_fix(&fix);
            }
        }

      elapsed = gnss_seconds_since(&start);
      if (fixes == 0 && elapsed - lastreport >= GNSS_STATUS_INTERVAL)
        {
          gnss_print_waiting(elapsed, inview);
          lastreport = elapsed;
        }
    }

  if (fixes == 0)
    {
      printf("No fix after %d s\n", timeout);
    }

out:
  if (fds[0].fd >= 0)
    {
      orb_unsubscribe(fds[0].fd);
    }

  if (fds[1].fd >= 0)
    {
      orb_unsubscribe(fds[1].fd);
    }

  return fixes > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
