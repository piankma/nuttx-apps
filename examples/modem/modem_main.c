/****************************************************************************
 * apps/examples/modem/modem_main.c
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
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/ioexpander/gpio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MODEM_LINE_MAX      256
#define MODEM_KEY_ON_MS     100    /* PWRKEY press that powers it on */
#define MODEM_KEY_OFF_MS    3000   /* PWRKEY press that powers it off */
#define MODEM_BOOT_MS       20000  /* To wait for the first answer */
#define MODEM_OFF_MS        10000  /* To wait for it to stop answering */
#define MODEM_SMS_MS        60000  /* To wait for an SMS to go out */
#define MODEM_CTRL_Z        0x1a   /* Ends the text of an SMS */
#define MODEM_TERM_EXIT     0x1d   /* Ctrl-] leaves the terminal */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct modem_s
{
  int fd;           /* The modem's serial port */
  int timeout;      /* Reply timeout in milliseconds */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR const char *g_regstat[] =
{
  "not registered", "registered, home", "searching", "denied",
  "unknown", "registered, roaming"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void modem_usage(FAR const char *progname)
{
  printf("Usage: %s [-t <s>] on|off|status|info\n"
         "       %s [-t <s>] at <command>\n"
         "       %s sms send <number> <text>...\n"
         "       %s sms list [all] | read <n> | delete <n>|all\n"
         "       %s term\n"
         "\n"
         "  on       power up, wait until it answers\n"
         "  off      shut down (AT+CPOF), then cut\n"
         "           its supply\n"
         "  status   supply, and whether it answers\n"
         "  info     identity, SIM, signal, network\n"
         "  at       send one command (no AT prefix\n"
         "           needed), print the reply\n"
         "  sms      send, list (unread, or all),\n"
         "           read and delete text messages\n"
         "  term     pass the console through;\n"
         "           Ctrl-] leaves\n"
         "  -t <s>   reply timeout (default 10)\n",
         progname, progname, progname, progname, progname);
}

static uint32_t modem_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void modem_sleep_ms(uint32_t ms)
{
  usleep(ms * 1000);
}

/* Read or write one of the board's GPIO devices: value < 0 reads */

static int modem_gpio(FAR const char *path, int value)
{
  bool level = false;
  int ret;
  int fd;

  fd = open(path, O_RDWR);
  if (fd < 0)
    {
      fprintf(stderr, "ERROR: cannot open %s: %d\n", path, errno);
      return -errno;
    }

  if (value < 0)
    {
      ret = ioctl(fd, GPIOC_READ, (unsigned long)((uintptr_t)&level));
      value = level;
    }
  else
    {
      ret = ioctl(fd, GPIOC_WRITE, (unsigned long)value);
    }

  close(fd);
  return ret < 0 ? -errno : value;
}

static bool modem_powered(void)
{
  return modem_gpio(CONFIG_EXAMPLES_MODEM_PWR, -1) > 0;
}

static void modem_presskey(uint32_t ms)
{
  modem_gpio(CONFIG_EXAMPLES_MODEM_PWRKEY, 1);
  modem_sleep_ms(ms);
  modem_gpio(CONFIG_EXAMPLES_MODEM_PWRKEY, 0);
}

/* Read one line from the modem, without its CR/LF, waiting at most
 * timeout ms.  Empty lines are skipped.  With prompt set, a '>' at the
 * start of a line (the SMS text prompt, which has no line end) is returned
 * as a line of its own.  Returns the length, or -ETIMEDOUT.
 */

static int modem_readline(FAR struct modem_s *modem, FAR char *line,
                          size_t size, int timeout, bool prompt)
{
  uint32_t end = modem_now_ms() + timeout;
  struct pollfd pfd;
  size_t len = 0;
  char ch;
  int left;

  for (; ; )
    {
      left = (int)(end - modem_now_ms());
      if (left <= 0)
        {
          return -ETIMEDOUT;
        }

      pfd.fd = modem->fd;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, left) <= 0 || read(modem->fd, &ch, 1) != 1)
        {
          continue;
        }

      if (ch == '\r' || ch == '\n')
        {
          if (len > 0)
            {
              line[len] = '\0';
              return len;
            }

          continue;
        }

      if (len < size - 1)
        {
          line[len++] = ch;
        }

      if (prompt && len == 1 && ch == '>')
        {
          line[len] = '\0';
          return len;
        }
    }
}

/* Print whatever the modem sent on its own (URCs such as RDY, +CPIN or
 * +CMTI for a new SMS), then return once it has been quiet for 50 ms.
 */

static void modem_drain(FAR struct modem_s *modem)
{
  char line[MODEM_LINE_MAX];

  while (modem_readline(modem, line, sizeof(line), 50, false) > 0)
    {
      printf("%s\n", line);
    }
}

static bool modem_final(FAR const char *line, FAR int *result)
{
  static FAR const char *errors[] =
  {
    "ERROR", "+CME ERROR", "+CMS ERROR", "NO CARRIER", "BUSY",
    "NO ANSWER", "NO DIALTONE"
  };

  int i;

  if (strcmp(line, "OK") == 0)
    {
      *result = 0;
      return true;
    }

  for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
    {
      if (strncmp(line, errors[i], strlen(errors[i])) == 0)
        {
          *result = -EIO;
          return true;
        }
    }

  return false;
}

static int modem_write(FAR struct modem_s *modem, FAR const char *data,
                       size_t len)
{
  while (len > 0)
    {
      ssize_t n = write(modem->fd, data, len);
      if (n < 0)
        {
          return -errno;
        }

      data += n;
      len  -= n;
    }

  return 0;
}

/* Wait for the final result of a command.  Every other line is handed to
 * reply (if not NULL) with arg, or printed.  The echo of cmd is skipped.
 */

static int modem_result(FAR struct modem_s *modem, FAR const char *cmd,
                        int timeout,
                        CODE void (*reply)(FAR const char *line,
                                           FAR void *arg),
                        FAR void *arg)
{
  char line[MODEM_LINE_MAX];
  int result;

  for (; ; )
    {
      if (modem_readline(modem, line, sizeof(line), timeout, false) < 0)
        {
          fprintf(stderr, "ERROR: no reply to %s\n", cmd ? cmd : "text");
          return -ETIMEDOUT;
        }

      if (cmd != NULL && strcmp(line, cmd) == 0)
        {
          continue;
        }

      if (modem_final(line, &result))
        {
          if (result < 0)
            {
              printf("%s\n", line);
            }

          return result;
        }

      if (reply != NULL)
        {
          reply(line, arg);
        }
      else
        {
          printf("%s\n", line);
        }
    }
}

/* Send one command (with "AT" in front unless it has it) and wait for its
 * final result.
 */

static int modem_cmd(FAR struct modem_s *modem, FAR const char *cmd,
                     int timeout,
                     CODE void (*reply)(FAR const char *line, FAR void *arg),
                     FAR void *arg)
{
  char buf[MODEM_LINE_MAX];
  int ret;

  if (strncasecmp(cmd, "AT", 2) == 0)
    {
      snprintf(buf, sizeof(buf), "%s", cmd);
    }
  else
    {
      snprintf(buf, sizeof(buf), "AT%s", cmd);
    }

  modem_drain(modem);

  ret = modem_write(modem, buf, strlen(buf));
  if (ret == 0)
    {
      ret = modem_write(modem, "\r", 1);
    }

  if (ret < 0)
    {
      fprintf(stderr, "ERROR: write failed: %d\n", ret);
      return ret;
    }

  return modem_result(modem, buf, timeout, reply, arg);
}

/* Send AT and see whether OK comes back within timeout ms */

static bool modem_answers(FAR struct modem_s *modem, int timeout)
{
  char line[MODEM_LINE_MAX];
  uint32_t end = modem_now_ms() + timeout;
  int result;
  int left;

  if (modem_write(modem, "AT\r", 3) < 0)
    {
      return false;
    }

  while ((left = (int)(end - modem_now_ms())) > 0)
    {
      if (modem_readline(modem, line, sizeof(line), left, false) < 0)
        {
          break;
        }

      if (modem_final(line, &result))
        {
          return result == 0;
        }

      if (strcmp(line, "AT") != 0)
        {
          printf("%s\n", line);
        }
    }

  return false;
}

static int modem_on(FAR struct modem_s *modem)
{
  uint32_t start;

  if (modem_powered() && modem_answers(modem, 1000))
    {
      printf("The modem is already on\n");
      return 0;
    }

  if (!modem_powered())
    {
      modem_gpio(CONFIG_EXAMPLES_MODEM_PWR, 1);
      modem_sleep_ms(100);
    }

  /* The supply alone does not start it: PWRKEY low for ~50 ms does, and
   * its UART is up about 8 s later.
   */

  start = modem_now_ms();
  modem_presskey(MODEM_KEY_ON_MS);
  printf("Pressed PWRKEY, waiting for the modem...\n");

  while (modem_now_ms() - start < MODEM_BOOT_MS)
    {
      if (modem_answers(modem, 1000))
        {
          printf("The modem answers after %.1f s\n",
                 (modem_now_ms() - start) / 1000.0);

          /* No command echo, and errors as text */

          modem_cmd(modem, "E0", modem->timeout, NULL, NULL);
          modem_cmd(modem, "+CMEE=2", modem->timeout, NULL, NULL);
          return 0;
        }
    }

  fprintf(stderr, "ERROR: no answer in %d s\n", MODEM_BOOT_MS / 1000);
  return -ETIMEDOUT;
}

static int modem_off(FAR struct modem_s *modem)
{
  uint32_t start;

  if (!modem_powered())
    {
      printf("The modem is already off\n");
      return 0;
    }

  /* Cutting its supply while it runs can damage its flash: shut it down
   * first, with AT+CPOF, or failing that with a long PWRKEY press.
   */

  start = modem_now_ms();
  if (modem_answers(modem, 1000))
    {
      modem_cmd(modem, "+CPOF", modem->timeout, NULL, NULL);
    }
  else
    {
      printf("No answer: holding PWRKEY for %d s\n",
             MODEM_KEY_OFF_MS / 1000);
      modem_presskey(MODEM_KEY_OFF_MS);
    }

  while (modem_now_ms() - start < MODEM_OFF_MS &&
         modem_answers(modem, 500))
    {
      modem_sleep_ms(500);
    }

  /* Its UART stops about 2 s before it is really down */

  modem_sleep_ms(2000);
  modem_gpio(CONFIG_EXAMPLES_MODEM_PWR, 0);
  printf("The modem is off (%.1f s)\n",
         (modem_now_ms() - start) / 1000.0);
  return 0;
}

static void modem_signal(FAR const char *line, FAR void *arg)
{
  int rssi;
  int ber;

  printf("%s", line);
  if (sscanf(line, "+CSQ: %d,%d", &rssi, &ber) == 2)
    {
      if (rssi == 99)
        {
          printf(" (no signal)");
        }
      else if (rssi <= 31)
        {
          printf(" (%d dBm)", -113 + 2 * rssi);
        }
    }

  printf("\n");
}

static void modem_registration(FAR const char *line, FAR void *arg)
{
  FAR const char *comma;
  int stat;

  printf("%s", line);

  /* +CREG: <n>,<stat>[,...]: the status is after the first comma */

  comma = strchr(line, ',');
  if (comma != NULL && sscanf(comma + 1, "%d", &stat) == 1 &&
      stat >= 0 && stat < sizeof(g_regstat) / sizeof(g_regstat[0]))
    {
      printf(" (%s)", g_regstat[stat]);
    }

  printf("\n");
}

static int modem_status(FAR struct modem_s *modem)
{
  if (!modem_powered())
    {
      printf("Supply off\n");
      return 0;
    }

  if (!modem_answers(modem, 1000))
    {
      printf("Supply on, no answer (off, or still starting)\n");
      return 0;
    }

  printf("Supply on, answering\n");
  modem_cmd(modem, "+CPIN?", modem->timeout, NULL, NULL);
  modem_cmd(modem, "+CSQ", modem->timeout, modem_signal, NULL);
  modem_cmd(modem, "+CEREG?", modem->timeout, modem_registration, NULL);
  return 0;
}

static int modem_info(FAR struct modem_s *modem)
{
  static FAR const char *cmds[] =
  {
    "I",          /* Manufacturer, model, revision, IMEI */
    "+CPIN?",     /* SIM state */
    "+CICCID",    /* SIM serial number */
    "+CIMI",      /* Subscriber identity */
    "+CNUM",      /* Own number, if the SIM has it */
    "+COPS?",     /* Operator */
    "+CPSI?",     /* Serving cell: technology, band, ... */
    "+CGDCONT?",  /* Data contexts (APN) */
    "+CGATT?",    /* Attached to packet data */
  };

  int i;

  if (!modem_answers(modem, 1000))
    {
      fprintf(stderr, "ERROR: the modem does not answer (modem on?)\n");
      return -ENODEV;
    }

  for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
    {
      modem_cmd(modem, cmds[i], modem->timeout, NULL, NULL);
      if (i == 1)
        {
          modem_cmd(modem, "+CSQ", modem->timeout, modem_signal, NULL);
          modem_cmd(modem, "+CREG?", modem->timeout, modem_registration,
                    NULL);
          modem_cmd(modem, "+CEREG?", modem->timeout, modem_registration,
                    NULL);
        }
    }

  return 0;
}

static int modem_sms_send(FAR struct modem_s *modem, FAR const char *number,
                          int argc, FAR char *argv[])
{
  char line[MODEM_LINE_MAX];
  int ret;
  int i;

  ret = modem_cmd(modem, "+CMGF=1", modem->timeout, NULL, NULL);
  if (ret == 0)
    {
      ret = modem_cmd(modem, "+CSCS=\"GSM\"", modem->timeout, NULL, NULL);
    }

  if (ret < 0)
    {
      return ret;
    }

  snprintf(line, sizeof(line), "AT+CMGS=\"%s\"\r", number);
  modem_drain(modem);
  modem_write(modem, line, strlen(line));

  /* The modem asks for the text with "> " */

  do
    {
      if (modem_readline(modem, line, sizeof(line), modem->timeout,
                         true) < 0)
        {
          fprintf(stderr, "ERROR: no text prompt\n");
          return -ETIMEDOUT;
        }

      if (modem_final(line, &ret))
        {
          printf("%s\n", line);
          return -EIO;
        }
    }
  while (line[0] != '>');

  for (i = 0; i < argc; i++)
    {
      if (i > 0)
        {
          modem_write(modem, " ", 1);
        }

      modem_write(modem, argv[i], strlen(argv[i]));
    }

  line[0] = MODEM_CTRL_Z;
  modem_write(modem, line, 1);

  ret = modem_result(modem, NULL, MODEM_SMS_MS, NULL, NULL);
  if (ret == 0)
    {
      printf("Sent\n");
    }

  return ret;
}

static int modem_sms(FAR struct modem_s *modem, int argc, FAR char *argv[])
{
  char cmd[32];

  if (argc < 1)
    {
      return -EINVAL;
    }

  if (strcmp(argv[0], "send") == 0 && argc >= 3)
    {
      return modem_sms_send(modem, argv[1], argc - 2, &argv[2]);
    }

  if (modem_cmd(modem, "+CMGF=1", modem->timeout, NULL, NULL) < 0)
    {
      return -EIO;
    }

  if (strcmp(argv[0], "list") == 0)
    {
      return modem_cmd(modem, argc > 1 && strcmp(argv[1], "all") == 0 ?
                       "+CMGL=\"ALL\"" : "+CMGL=\"REC UNREAD\"",
                       modem->timeout * 2, NULL, NULL);
    }

  if (strcmp(argv[0], "read") == 0 && argc == 2)
    {
      snprintf(cmd, sizeof(cmd), "+CMGR=%d", atoi(argv[1]));
      return modem_cmd(modem, cmd, modem->timeout, NULL, NULL);
    }

  if (strcmp(argv[0], "delete") == 0 && argc == 2)
    {
      if (strcmp(argv[1], "all") == 0)
        {
          snprintf(cmd, sizeof(cmd), "+CMGD=1,4");
        }
      else
        {
          snprintf(cmd, sizeof(cmd), "+CMGD=%d", atoi(argv[1]));
        }

      return modem_cmd(modem, cmd, modem->timeout, NULL, NULL);
    }

  return -EINVAL;
}

/* Pass the console through to the modem until Ctrl-] */

static int modem_term(FAR struct modem_s *modem)
{
  struct pollfd pfd[2];
  bool lastcr = false;
  char buf[64];
  ssize_t n;
  int i;

  printf("Connected to %s; Ctrl-] leaves\n", CONFIG_EXAMPLES_MODEM_TTY);
  fflush(stdout);

  /* The console already echoes what is typed, so keep the modem from
   * echoing it a second time.
   */

  modem_cmd(modem, "E0", modem->timeout, NULL, NULL);

  pfd[0].fd = STDIN_FILENO;
  pfd[0].events = POLLIN;
  pfd[1].fd = modem->fd;
  pfd[1].events = POLLIN;

  for (; ; )
    {
      if (poll(pfd, 2, -1) < 0)
        {
          continue;
        }

      if (pfd[1].revents & POLLIN)
        {
          n = read(modem->fd, buf, sizeof(buf));
          if (n > 0)
            {
              write(STDOUT_FILENO, buf, n);
            }
        }

      if (pfd[0].revents & POLLIN)
        {
          n = read(STDIN_FILENO, buf, sizeof(buf));
          for (i = 0; i < n; i++)
            {
              if (buf[i] == MODEM_TERM_EXIT)
                {
                  printf("\n");
                  return 0;
                }

              /* Lines end in CR, and a CR LF pair is only one line end */

              if (buf[i] == '\r' || buf[i] == '\n')
                {
                  if (buf[i] == '\n' && lastcr)
                    {
                      lastcr = false;
                      continue;
                    }

                  lastcr = buf[i] == '\r';
                  modem_write(modem, "\r", 1);
                }
              else
                {
                  lastcr = false;
                  modem_write(modem, &buf[i], 1);
                }
            }
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct modem_s modem =
    {
      .timeout = 10000,
    };

  FAR const char *cmd;
  int ret;
  int ch;

  while ((ch = getopt(argc, argv, "t:h")) != ERROR)
    {
      switch (ch)
        {
          case 't':
            modem.timeout = atoi(optarg) * 1000;
            break;

          default:
            modem_usage(argv[0]);
            return ch == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

  if (optind >= argc || modem.timeout <= 0)
    {
      modem_usage(argv[0]);
      return EXIT_FAILURE;
    }

  cmd = argv[optind];

  /* The supply comes first: while it is off, the level shifter between
   * the modem and its UART is unpowered.
   */

  if (strcmp(cmd, "on") != 0 && strcmp(cmd, "off") != 0 &&
      strcmp(cmd, "status") != 0 && !modem_powered())
    {
      fprintf(stderr, "ERROR: the modem is off (modem on)\n");
      return EXIT_FAILURE;
    }

  modem.fd = open(CONFIG_EXAMPLES_MODEM_TTY, O_RDWR);
  if (modem.fd < 0)
    {
      fprintf(stderr, "ERROR: cannot open %s: %d\n",
              CONFIG_EXAMPLES_MODEM_TTY, errno);
      return EXIT_FAILURE;
    }

  if (strcmp(cmd, "on") == 0)
    {
      ret = modem_on(&modem);
    }
  else if (strcmp(cmd, "off") == 0)
    {
      ret = modem_off(&modem);
    }
  else if (strcmp(cmd, "status") == 0)
    {
      ret = modem_status(&modem);
    }
  else if (strcmp(cmd, "info") == 0)
    {
      ret = modem_info(&modem);
    }
  else if (strcmp(cmd, "at") == 0 && optind + 1 < argc)
    {
      ret = modem_cmd(&modem, argv[optind + 1], modem.timeout, NULL, NULL);
    }
  else if (strcmp(cmd, "sms") == 0)
    {
      ret = modem_sms(&modem, argc - optind - 1, &argv[optind + 1]);
      if (ret == -EINVAL)
        {
          modem_usage(argv[0]);
        }
    }
  else if (strcmp(cmd, "term") == 0)
    {
      ret = modem_term(&modem);
    }
  else
    {
      modem_usage(argv[0]);
      ret = -EINVAL;
    }

  close(modem.fd);
  return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
