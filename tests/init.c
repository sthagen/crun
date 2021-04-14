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
#define _GNU_SOURCE

#include <config.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/prctl.h>

#ifdef HAVE_SECCOMP
# include <linux/seccomp.h>
# include <linux/filter.h>
# include <seccomp.h>

# ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#  define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
# endif

# ifndef SECCOMP_SET_MODE_FILTER
#  define SECCOMP_SET_MODE_FILTER 1
# endif

#endif

#ifdef HAVE_ERROR_H
# include <error.h>
#else
# define error(status, errno, fmt, ...) do {                            \
    if (errno == 0)                                                     \
      fprintf (stderr, "crun: " fmt "\n", ##__VA_ARGS__);               \
    else                                                                \
      {                                                                 \
        fprintf (stderr, "crun: " fmt, ##__VA_ARGS__);                  \
        fprintf (stderr, ": %s\n", strerror (errno));                   \
      }                                                                 \
    if (status)                                                         \
      exit (status);                                                    \
  } while(0)
#endif

static int
cat (char *file)
{
  FILE *f = fopen (file, "rb");
  char buf[512];
  if (f == NULL)
    error (EXIT_FAILURE, errno, "fopen");
  while (1)
    {
      size_t s = fread (buf, 1, sizeof (buf), f);
      if (s == 0)
        {
          if (feof (f))
            {
              fclose (f);
              return 0;
            }
          fclose (f);
          error (EXIT_FAILURE, errno, "fread");
        }
      s = fwrite (buf, 1, s, stdout);
      if (s == 0)
        error (EXIT_FAILURE, errno, "fwrite");
    }
}

static int
open_only (char *file)
{
  int fd = open (file, O_RDONLY);
  if (fd >= 0)
    {
      close (fd);
      exit (0);
    }
  error (EXIT_FAILURE, errno, "could not open %s", file);
  return -1;
}

static int
write_to (const char *path, const char *str)
{
  FILE *f = fopen (path, "wb");
  int ret;
  if (f == NULL)
    error (EXIT_FAILURE, errno, "fopen");
  ret = fprintf (f, "%s", str);
  if (ret < 0)
    error (EXIT_FAILURE, errno, "fprintf");
  ret = fclose (f);
  if (ret)
    error (EXIT_FAILURE, errno, "fclose");
  return 0;
}

static int
ls (char *path)
{
  DIR *dir = opendir (path);
  if (dir == NULL)
    error (EXIT_FAILURE, errno, "opendir");

  for (;;)
    {
      struct dirent *de;
      errno = 0;
      de = readdir (dir);
      if (de == NULL && errno)
        error (EXIT_FAILURE, errno, "readdir");
      if (de == NULL)
        {
          closedir (dir);
          return 0;
        }
      printf ("%s\n", de->d_name);
    }
  closedir (dir);
  return 0;
}

static int
sd_notify ()
{
  int ret;
  int notify_socket_fd;
  char *notify_socket_name;
  struct sockaddr_un notify_socket_unix_name;
  const char *ready_data = "READY=1";
  const int ready_data_len = 7;

  notify_socket_name = getenv ("NOTIFY_SOCKET");

  if (notify_socket_name == NULL)
    error (EXIT_FAILURE, 0, "NOTIFY_SOCKET not found in environment");

  notify_socket_fd = socket (AF_UNIX, SOCK_DGRAM, 0);
  if (notify_socket_fd < 0)
    error (EXIT_FAILURE, errno, "socket");

  notify_socket_unix_name.sun_family = AF_UNIX;
  strncpy (notify_socket_unix_name.sun_path, notify_socket_name,
           sizeof(notify_socket_unix_name.sun_path));

  ret = sendto (notify_socket_fd, ready_data, ready_data_len, 0,
                (struct sockaddr *)&notify_socket_unix_name, sizeof(notify_socket_unix_name));

  if (ret < 0)
    error (EXIT_FAILURE, 0, "sendto");

  return 0;
}

static int
syscall_seccomp (unsigned int operation, unsigned int flags, void *args)
{
  return (int) syscall (__NR_seccomp, operation, flags, args);
}

int main (int argc, char **argv)
{
  if (argc < 2)
    error (EXIT_FAILURE, 0, "specify at least one command");

  if (strcmp (argv[1], "true") == 0)
    {
      exit (0);
    }

  if (strcmp (argv[1], "echo") == 0)
    {
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'echo' requires an argument");
      fputs (argv[2], stdout);
      exit (0);
    }

  if (strcmp (argv[1], "printenv") == 0)
    {
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'printenv' requires an argument");
      fputs (getenv (argv[2]), stdout);
      exit (0);
    }

  if (strcmp (argv[1], "groups") == 0)
    {
      gid_t groups[10];
      int max_groups = sizeof(groups) / sizeof(groups[0]);
      int n_groups, i;
      n_groups = getgroups(max_groups, groups);
      fputs("GROUPS=[", stdout);
      for (i = 0; i < n_groups; i++)
        printf("%s%d", i == 0 ? "" : " ", groups[i]);
      fputs("]\n", stdout);
      exit (0);
    }

  if (strcmp (argv[1], "cat") == 0)
    {
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'cat' requires an argument");
      return cat (argv[2]);
    }

  if (strcmp (argv[1], "open") == 0)
    {
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'open' requires an argument");
      return open_only (argv[2]);
    }

  if (strcmp (argv[1], "access") == 0)
    {
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'access' requires an argument");

      if (access (argv[2], F_OK) < 0)
        error (EXIT_FAILURE, errno, "could not access %s", argv[2]);
      return 0;
    }

  if (strcmp (argv[1], "cwd") == 0)
    {
      int ret;
      char *wd = getcwd (NULL, 0);
      if (wd == NULL)
        error (EXIT_FAILURE, 0, "OOM");

      ret = printf ("%s\n", wd);
      if (ret < 0)
        error (EXIT_FAILURE, errno, "printf");
      return 0;
    }

  if (strcmp (argv[1], "gethostname") == 0)
    {
      char buffer[64] = {};
      int ret;

      ret = gethostname (buffer, sizeof (buffer) - 1);
      if (ret < 0)
        error (EXIT_FAILURE, errno, "gethostname");

      ret = printf ("%s\n", buffer);
      if (ret < 0)
        error (EXIT_FAILURE, errno, "printf");
      return 0;
    }

  if (strcmp (argv[1], "isatty") == 0)
    {
      int fd;
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'isatty' requires two arguments");
      fd = atoi (argv[2]);
      printf (isatty (fd) ? "true" : "false");
      return 0;
    }

  if (strcmp (argv[1], "write") == 0)
    {
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'write' requires two arguments");
      return write_to (argv[2], argv[3]);
    }
  if (strcmp (argv[1], "pause") == 0)
    {
      unsigned int remaining = 120;

      close (1);
      close (2);

      while (remaining)
        remaining = sleep (remaining);

      exit (0);
    }
  if (strcmp (argv[1], "forkbomb") == 0)
    {
      int i, n;
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'forkbomb' requires two arguments");
      n = atoi (argv[2]);
      if (n < 0)
        return 0;
      for (i = 0; i < n; i++)
        {
          pid_t pid = fork ();
          if (pid < 0)
            error (EXIT_FAILURE, errno, "fork");
          if (pid == 0)
            sleep (100);
        }

      return 0;
    }

  if (strcmp (argv[1], "ls") == 0)
    {
      /* Fork so that ls /proc/1/fd doesn't show more fd's.  */
      pid_t pid;
      if (argc < 3)
        error (EXIT_FAILURE, 0, "'ls' requires two arguments");
      pid = fork ();
      if (pid < 0)
        error (EXIT_FAILURE, errno, "fork");
      if (pid)
        {
          int ret, status;
          do
            ret = waitpid (pid, &status, 0);
          while (ret < 0 && errno == EINTR);
          if (ret < 0)
            return ret;
          return status;
        }
      return ls (argv[2]);
    }

  if (strcmp (argv[1], "systemd-notify") == 0)
    return sd_notify();

  if (strcmp (argv[1], "check-feature") == 0)
    {
      if (argc < 3)
        error (EXIT_FAILURE, 0, "`check-feature` requires an argument");

      if (strcmp (argv[2], "open_tree") == 0)
        {
#if defined __NR_open_tree
          int ret;

          ret = syscall(__NR_open_tree);
          return (ret >= 0 || errno != ENOSYS) ? 0 : 1;
#else
          return 1;
#endif
        }
      else if (strcmp (argv[2], "move_mount") == 0)
        {
#if defined __NR_move_mount
          int ret;

          ret = syscall(__NR_move_mount);
          return (ret >= 0 || errno != ENOSYS) ? 0 : 1;
#else
          return 1;
#endif
        }
      else if (strcmp (argv[2], "seccomp-listener") == 0)
        {
#ifdef HAVE_SECCOMP
          int ret;
          int p = fork ();
          if (p < 0)
            return 1;
          if (p)
            {
              int status;
              do
                ret = waitpid (p, &status, 0);
              while (ret < 0 && errno == EINTR);
              if (ret == p && WIFEXITED (status) && WEXITSTATUS (status) == 0)
                return 0;

              return 1;
            }
          else
            {
              struct sock_fprog seccomp_filter;
              const char bpf[] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x7f};
              seccomp_filter.len = 1;
              seccomp_filter.filter = (struct sock_filter *) bpf;

              if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
                return 1;

              ret = syscall_seccomp (SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &seccomp_filter);
              if (ret <= 0)
                return 1;

              return 0;
            }
#endif
          return 1;
        }
      else
        error (EXIT_FAILURE, 0, "unknown feature");
    }

  error (EXIT_FAILURE, 0, "unknown command '%s' specified", argv[1]);
  return 0;
}
