/****************************************************************************
 * apps/examples/haptic/haptic_main.c
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
 * Play one effect on a force feedback vibration motor, wait for it to end,
 * and erase it again.  Uploaded effects outlive the file that uploaded
 * them, so a program that leaves without erasing uses up the device's
 * effect slots for good.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/input/ff.h>
#ifdef CONFIG_FF_DRV2605
#  include <nuttx/input/drv2605.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HAPTIC_SEQUENCE_LEN  8      /* Built-in effects played in a row */
#define HAPTIC_POLL_MS       20
#define HAPTIC_TIMEOUT_MS    10000  /* Longest wait for an effect to end */
#define HAPTIC_EFFECT_MS     1000   /* Wait without a way to ask */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: show_usage
 ****************************************************************************/

static void show_usage(FAR const char *progname)
{
  fprintf(stderr,
          "Usage: %s [-d <dev>] [buzz [<ms> [<percent>]]]\n"
          "       %s [-d <dev>] effect <n> [<n> ...]\n"
          "\n"
          "  buzz    Vibrate for <ms> milliseconds (default 200) at\n"
          "          <percent> strength (default 100).\n"
          "  effect  Play up to %d of the driver's built-in effects in a\n"
          "          row.  On a DRV2605: 1 strong click, 7 soft bump,\n"
          "          10 double click, 14 strong buzz, 47 buzz.\n"
          "  -d      Device, default %s\n",
          progname, progname, HAPTIC_SEQUENCE_LEN,
          CONFIG_EXAMPLES_HAPTIC_DEVPATH);
}

/****************************************************************************
 * Name: wait_effect
 *
 * Description:
 *   Wait for the effect to end.  A DRV2605 can be asked; otherwise wait out
 *   the time the effect is known, or assumed, to take.
 *
 ****************************************************************************/

static void wait_effect(int fd, int ms)
{
#ifdef DRV2605IOC_BUSY
  int waited;
  int busy;

  for (waited = 0; waited < HAPTIC_TIMEOUT_MS; waited += HAPTIC_POLL_MS)
    {
      if (ioctl(fd, DRV2605IOC_BUSY, (unsigned long)(uintptr_t)&busy) < 0)
        {
          break;
        }

      if (!busy)
        {
          return;
        }

      usleep(HAPTIC_POLL_MS * 1000);
    }

  if (waited > 0)
    {
      return;
    }
#endif

  usleep(ms * 1000);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *devpath = CONFIG_EXAMPLES_HAPTIC_DEVPATH;
  FAR const char *mode = "buzz";
  int16_t sequence[HAPTIC_SEQUENCE_LEN];
  struct ff_effect effect;
  struct ff_event_s play;
  int waitms;
  int opt;
  int fd;
  int i;

  while ((opt = getopt(argc, argv, "d:h")) != -1)
    {
      switch (opt)
        {
          case 'd':
            devpath = optarg;
            break;

          default:
            show_usage(argv[0]);
            return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

  if (optind < argc)
    {
      mode = argv[optind++];
    }

  memset(&effect, 0, sizeof(effect));
  effect.id = -1;

  if (strcmp(mode, "buzz") == 0)
    {
      int ms = optind < argc ? atoi(argv[optind++]) : 200;
      int percent = optind < argc ? atoi(argv[optind++]) : 100;

      if (ms < 1 || ms > 65535 || percent < 0 || percent > 100 ||
          optind < argc)
        {
          show_usage(argv[0]);
          return EXIT_FAILURE;
        }

      effect.type = FF_CONSTANT;
      effect.u.constant.level = percent * 0x7fff / 100;
      effect.replay.length = ms;
      waitms = ms;
    }
  else if (strcmp(mode, "effect") == 0)
    {
      int count = argc - optind;

      if (count < 1 || count > HAPTIC_SEQUENCE_LEN)
        {
          show_usage(argv[0]);
          return EXIT_FAILURE;
        }

      for (i = 0; i < count; i++)
        {
          sequence[i] = atoi(argv[optind + i]);
        }

      effect.type = FF_PERIODIC;
      effect.u.periodic.waveform = FF_CUSTOM;
      effect.u.periodic.custom_len = count;
      effect.u.periodic.custom_data = sequence;
      waitms = HAPTIC_EFFECT_MS;
    }
  else
    {
      show_usage(argv[0]);
      return EXIT_FAILURE;
    }

  fd = open(devpath, O_RDWR);
  if (fd < 0)
    {
      fprintf(stderr, "ERROR: Failed to open %s: %d\n", devpath, errno);
      return EXIT_FAILURE;
    }

  if (ioctl(fd, EVIOCSFF, (unsigned long)(uintptr_t)&effect) < 0)
    {
      fprintf(stderr, "ERROR: The device rejected the effect: %d\n",
              errno);
      close(fd);
      return EXIT_FAILURE;
    }

  play.code  = effect.id;
  play.value = 1;
  write(fd, &play, sizeof(play));

  wait_effect(fd, waitms);

  ioctl(fd, EVIOCRMFF, effect.id);
  close(fd);
  return EXIT_SUCCESS;
}
