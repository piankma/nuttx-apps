/****************************************************************************
 * apps/examples/lora/lora_main.c
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

#include <sys/ioctl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/wireless/ioctl.h>
#include <nuttx/wireless/lpwan/sx126x.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LORA_MAX_PAYLOAD 255

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct lora_bandwidth_s
{
  FAR const char *name;
  uint32_t hz;
  enum sx126x_lora_bw_e bw;
};

struct lora_options_s
{
  uint32_t freq;
  int sf;
  int bw;                             /* Index into g_bandwidths */
  int cr;                             /* 5-8, for 4/5-4/8 */
  int power;
  uint16_t syncword;
  int preamble;
  int count;
  int seconds;
  bool hex;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct lora_bandwidth_s g_bandwidths[] =
{
  { "7.8",   7810,   SX126X_LORA_BW_7   },
  { "10.4",  10420,  SX126X_LORA_BW_10  },
  { "15.6",  15630,  SX126X_LORA_BW_15  },
  { "20.8",  20830,  SX126X_LORA_BW_20  },
  { "31.25", 31250,  SX126X_LORA_BW_31  },
  { "41.7",  41670,  SX126X_LORA_BW_41  },
  { "62.5",  62500,  SX126X_LORA_BW_62  },
  { "125",   125000, SX126X_LORA_BW_125 },
  { "250",   250000, SX126X_LORA_BW_250 },
  { "500",   500000, SX126X_LORA_BW_500 },
};

#define LORA_NBANDWIDTHS (sizeof(g_bandwidths) / sizeof(g_bandwidths[0]))
#define LORA_BW_125      7

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void lora_usage(FAR const char *progname)
{
  printf("Usage: %s [options] tx <text>...\n"
         "       %s [options] rx\n"
         "\n"
         "  -f <Hz>     frequency (default 869525000)\n"
         "  -s <5-12>   spreading factor (default 9)\n"
         "  -b <kHz>    bandwidth: 7.8 10.4 15.6 20.8 31.25 41.7 62.5 125\n"
         "              250 500 (default 125)\n"
         "  -c <5-8>    coding rate 4/5 to 4/8 (default 5)\n"
         "  -p <dBm>    transmit power, -9 to 22 (default 14)\n"
         "  -w <word>   sync word: one byte as other LoRa chips take it\n"
         "              (0x12 private, the default; 0x34 LoRaWAN), or the\n"
         "              SX126x's two byte register value (0x1424)\n"
         "  -l <n>      preamble length in symbols (default 8)\n"
         "  -n <n>      rx: stop after n packets (default 0, no limit)\n"
         "  -t <s>      rx: stop after s seconds (default 60, 0 = never)\n"
         "  -x          rx: print payloads in hex; tx: the payload is\n"
         "              given in hex\n"
         "\n"
         "Mind the duty cycle limits of the band: 869.4-869.65 MHz\n"
         "allows up to 27 dBm ERP at 10%%, most of the rest of 863-870 MHz\n"
         "14 dBm at 1%% or less.\n",
         progname, progname);
}

static int lora_configure(int fd, FAR const struct lora_options_s *opt)
{
  struct sx126x_lora_config_s config;
  uint32_t symbol_us;
  int8_t power = opt->power;

  /* Long symbols need the low data rate optimisation, on both ends */

  symbol_us = (uint32_t)(((uint64_t)1000000 << opt->sf) /
                         g_bandwidths[opt->bw].hz);

  memset(&config, 0, sizeof(config));
  config.modulation.spreading_factor = opt->sf;
  config.modulation.bandwidth = g_bandwidths[opt->bw].bw;
  config.modulation.coding_rate = opt->cr - 4;
  config.modulation.low_datarate_optimization = symbol_us > 16000;
  config.packet.preambles = opt->preamble;
  config.packet.fixed_length_header = false;
  config.packet.payload_length = LORA_MAX_PAYLOAD;
  config.packet.crc_enable = true;
  config.packet.invert_iq = false;

  if (ioctl(fd, SX126XIOC_LORACONFIGSET, (unsigned long)&config) < 0 ||
      ioctl(fd, SX126XIOC_SYNCWORDSET, (unsigned long)&opt->syncword) < 0 ||
      ioctl(fd, WLIOC_SETTXPOWER, (unsigned long)&power) < 0)
    {
      fprintf(stderr, "ERROR: configuration failed: %d\n", errno);
      return -1;
    }

  if (ioctl(fd, WLIOC_SETRADIOFREQ, (unsigned long)&opt->freq) < 0)
    {
      fprintf(stderr, "ERROR: %lu Hz is not allowed on this board\n",
              (unsigned long)opt->freq);
      return -1;
    }

  printf("%lu Hz, SF%d, %s kHz, CR 4/%d, sync 0x%04x, preamble %d%s\n",
         (unsigned long)opt->freq, opt->sf, g_bandwidths[opt->bw].name,
         opt->cr, opt->syncword, opt->preamble,
         config.modulation.low_datarate_optimization ? ", LDRO" : "");
  return 0;
}

static uint32_t lora_ms_since(FAR const struct timespec *start)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - start->tv_sec) * 1000 +
         (now.tv_nsec - start->tv_nsec) / 1000000;
}

static int lora_tx(int fd, FAR const struct lora_options_s *opt,
                   int argc, FAR char **argv)
{
  char payload[LORA_MAX_PAYLOAD + 1];
  struct timespec start;
  size_t len = 0;
  ssize_t ret;
  int i;

  if (argc < 1)
    {
      fprintf(stderr, "ERROR: nothing to send\n");
      return -1;
    }

  if (opt->hex)
    {
      FAR const char *hex = argv[0];
      unsigned int byte;

      while (hex[0] != '\0' && hex[1] != '\0' && len < LORA_MAX_PAYLOAD &&
             sscanf(hex, "%2x", &byte) == 1)
        {
          payload[len++] = byte;
          hex += 2;
        }

      if (hex[0] != '\0')
        {
          fprintf(stderr, "ERROR: bad hex payload\n");
          return -1;
        }
    }
  else
    {
      for (i = 0; i < argc; i++)
        {
          len += snprintf(payload + len, sizeof(payload) - len, "%s%s",
                          i > 0 ? " " : "", argv[i]);
          if (len >= LORA_MAX_PAYLOAD)
            {
              len = LORA_MAX_PAYLOAD;
              break;
            }
        }
    }

  printf("TX %d dBm, %zu bytes\n", opt->power, len);

  clock_gettime(CLOCK_MONOTONIC, &start);
  ret = write(fd, payload, len);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: transmission failed: %d\n", errno);
      return -1;
    }

  printf("Sent in %lu ms\n", (unsigned long)lora_ms_since(&start));
  return 0;
}

static void lora_print(FAR const struct sx126x_read_header_s *packet,
                       int n, bool hex)
{
  bool text = !hex;
  int snr = packet->snr;
  int i;

  for (i = 0; text && i < packet->payload_length; i++)
    {
      text = isprint(packet->payload[i]);
    }

  printf("#%d %u bytes, RSSI %d dBm, SNR %s%d.%02d dB%s: ", n,
         packet->payload_length, packet->rssi_db, snr < 0 ? "-" : "",
         abs(snr) / 4, abs(snr) % 4 * 25,
         packet->crc_error ? ", CRC ERROR" : "");

  if (text)
    {
      printf("\"%.*s\"\n", packet->payload_length, packet->payload);
      return;
    }

  for (i = 0; i < packet->payload_length; i++)
    {
      printf("%02x", packet->payload[i]);
    }

  printf("\n");
}

static int lora_rx(int fd, FAR const struct lora_options_s *opt)
{
  struct sx126x_read_header_s packet;
  struct timespec start;
  uint32_t timeout;
  uint32_t elapsed;
  ssize_t ret;
  int n = 0;

  if (opt->seconds > 0)
    {
      printf("Listening for %d s\n", opt->seconds);
    }
  else
    {
      printf("Listening\n");
    }

  clock_gettime(CLOCK_MONOTONIC, &start);

  while (opt->count == 0 || n < opt->count)
    {
      timeout = 0;
      if (opt->seconds > 0)
        {
          elapsed = lora_ms_since(&start);
          if (elapsed >= (uint32_t)opt->seconds * 1000)
            {
              break;
            }

          timeout = (uint32_t)opt->seconds * 1000 - elapsed;
        }

      ioctl(fd, SX126XIOC_RXTIMEOUTSET, (unsigned long)&timeout);

      ret = read(fd, &packet, sizeof(packet));
      if (ret < 0)
        {
          if (errno == ETIMEDOUT)
            {
              break;
            }

          fprintf(stderr, "ERROR: reception failed: %d\n", errno);
          return -1;
        }

      lora_print(&packet, ++n, opt->hex);
    }

  printf("%d packet%s\n", n, n == 1 ? "" : "s");
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct lora_options_s opt =
    {
      .freq     = 869525000,
      .sf       = 9,
      .bw       = LORA_BW_125,
      .cr       = 5,
      .power    = 14,
      .syncword = 0x1424,
      .preamble = 8,
      .count    = 0,
      .seconds  = 60,
      .hex      = false,
    };

  unsigned long value;
  int ret;
  int fd;
  int ch;
  int i;

  while ((ch = getopt(argc, argv, "f:s:b:c:p:w:l:n:t:xh")) != ERROR)
    {
      switch (ch)
        {
          case 'f':
            opt.freq = strtoul(optarg, NULL, 0);
            break;

          case 's':
            opt.sf = atoi(optarg);
            break;

          case 'b':
            for (i = 0; i < LORA_NBANDWIDTHS; i++)
              {
                if (strcmp(optarg, g_bandwidths[i].name) == 0 ||
                    atoi(optarg) == (int)(g_bandwidths[i].hz / 1000))
                  {
                    break;
                  }
              }

            opt.bw = i;
            break;

          case 'c':
            opt.cr = atoi(optarg);
            break;

          case 'p':
            opt.power = atoi(optarg);
            break;

          case 'w':
            value = strtoul(optarg, NULL, 0);
            if (value <= 0xff)
              {
                /* 0xXY as other LoRa chips take it is 0xX4Y4 here */

                value = (value & 0xf0) << 8 | 0x0400 |
                        (value & 0x0f) << 4 | 0x04;
              }

            opt.syncword = value;
            break;

          case 'l':
            opt.preamble = atoi(optarg);
            break;

          case 'n':
            opt.count = atoi(optarg);
            break;

          case 't':
            opt.seconds = atoi(optarg);
            break;

          case 'x':
            opt.hex = true;
            break;

          default:
            lora_usage(argv[0]);
            return ch == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

  if (optind >= argc || opt.sf < 5 || opt.sf > 12 ||
      opt.bw >= LORA_NBANDWIDTHS || opt.cr < 5 || opt.cr > 8 ||
      opt.preamble < 1 || opt.count < 0 || opt.seconds < 0)
    {
      lora_usage(argv[0]);
      return EXIT_FAILURE;
    }

  fd = open(CONFIG_EXAMPLES_LORA_DEVPATH, O_RDWR);
  if (fd < 0)
    {
      fprintf(stderr, "ERROR: cannot open %s: %d\n",
              CONFIG_EXAMPLES_LORA_DEVPATH, errno);
      return EXIT_FAILURE;
    }

  ret = lora_configure(fd, &opt);
  if (ret == 0)
    {
      if (strcmp(argv[optind], "tx") == 0)
        {
          ret = lora_tx(fd, &opt, argc - optind - 1, &argv[optind + 1]);
        }
      else if (strcmp(argv[optind], "rx") == 0)
        {
          ret = lora_rx(fd, &opt);
        }
      else
        {
          lora_usage(argv[0]);
          ret = -1;
        }
    }

  close(fd);
  return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
