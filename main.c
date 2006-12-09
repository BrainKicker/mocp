/*
 * MOC - music on console
 * Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#define _GNU_SOURCE
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include "server.h"
#include "common.h"
#include "interface.h"
#include "options.h"
#include "protocol.h"
#include "log.h"
#include "compat.h"
#include "common.h"
#include "decoder.h"

struct parameters
{
	int debug;
	int only_server;
	int foreground;
	int append;
	int clear;
	int play;
	int dont_run_iface;
	int dont_run_server;
	int stop;
	int exit;
	int pause;
	int unpause;
	int next;
	int previous;
	int get_file_info;
	int toggle_pause;
	int playit;
	int seek_by;
	char *adj_volume;
};


/* Connect to the server, return fd os the socket or -1 on error */
static int server_connect ()
{
	struct sockaddr_un sock_name;
	int sock;
	
	/* Create a socket */
	if ((sock = socket (PF_LOCAL, SOCK_STREAM, 0)) == -1)
		 return -1;
	
	sock_name.sun_family = AF_LOCAL;
	strcpy (sock_name.sun_path, socket_name());

	if (connect(sock, (struct sockaddr *)&sock_name,
				SUN_LEN(&sock_name)) == -1) {
		close (sock);
		return -1;
	}

	return sock;
}

/* Ping the server. return 1 if the server respond with EV_PONG, otherwise 1. */
static int ping_server (int sock)
{
	int event;
	
	send_int(sock, CMD_PING); /* ignore errors - the server could have
				     already closed the connection and sent
				     EV_BUSY */
	if (!get_int(sock, &event))
		fatal ("Error when receiving pong response.");
	return event == EV_PONG ? 1 : 0;
}

/* Check if a directory ./.moc exists and create if needed. */
static void check_moc_dir ()
{
	char *dir_name = create_file_name ("");
	struct stat file_stat;

	/* strip trailing slash */
	dir_name[strlen(dir_name)-1] = 0;

	if (stat(dir_name, &file_stat) == -1) {
		if (errno == ENOENT) {
			if (mkdir(dir_name, 0700) == -1)
				fatal ("Can't create directory %s, %s",
						dir_name, strerror(errno));
		}
		else
			fatal ("Error trying to check for "CONFIG_DIR
					" directory: %s", strerror(errno));
	}
}

static void sig_chld (int sig ATTR_UNUSED)
{
	logit ("Got SIGCHLD");

	waitpid(-1, NULL, WNOHANG);
}

/* Run client and the server if needed. */
static void start_moc (const struct parameters *params, char **args,
		const int arg_num)
{
	int list_sock;
	int server_sock = -1;

	decoder_init (params->debug);
	srand (time(NULL));

	if (!params->foreground && (server_sock = server_connect()) == -1) {
		int notify_pipe[2];
		int i = 0;

		printf ("Running the server...\n");

		/* To notify the client that the server socket is ready */
		if (pipe(notify_pipe))
			fatal ("pipe() failed: %s", strerror(errno));

		switch (fork()) {
			case 0: /* child - start server */
				set_me_server ();
				list_sock = server_init (params->debug,
						params->foreground);
				write (notify_pipe[1], &i, sizeof(i));
				close (notify_pipe[0]);
				close (notify_pipe[1]);
				server_loop (list_sock);
				options_free ();
				exit (0);
			case -1:
				fatal ("fork() failed");
			default:
				close (notify_pipe[1]);
				if (read(notify_pipe[0], &i, sizeof(i))
						!= sizeof(i))
					fatal ("Server exited");
				close (notify_pipe[0]);
				if ((server_sock = server_connect()) == -1) {
					perror ("server_connect()");
					fatal ("Can't connect to the server");
				}
		}
	}
	else if (!params->foreground && params->only_server)
		fatal ("Server is already running");
	else if (params->foreground && params->only_server) {
		set_me_server ();
		list_sock = server_init (params->debug, params->foreground);
		server_loop (list_sock);
	}

	signal (SIGCHLD, sig_chld);

	if (!params->only_server) {
		signal (SIGPIPE, SIG_IGN);
		if (ping_server(server_sock)) {
			if (!params->dont_run_iface) {
				init_interface (server_sock, params->debug,
						args, arg_num);
				interface_loop ();
				interface_end ();
			}
		}
		else
			fatal ("Can't connect to the server.");
	}

	if (params->dont_run_iface) {
		if (params->playit)
			interface_cmdline_playit (server_sock, args, arg_num);
		if (params->clear)
			interface_cmdline_clear_plist (server_sock);
		if (params->append)
			interface_cmdline_append (server_sock, args, arg_num);
		if (params->play)
			interface_cmdline_play_first (server_sock);
		if (params->get_file_info)
			interface_cmdline_file_info (server_sock);
		if (params->seek_by)
			interface_cmdline_seek_by (server_sock,
					params->seek_by);
		if (params->adj_volume)
			interface_cmdline_adj_volume (server_sock,
					params->adj_volume);
		send_int (server_sock, CMD_DISCONNECT);
	}
	else if (params->only_server)
		send_int (server_sock, CMD_DISCONNECT);

	
	options_free ();
	decoder_cleanup ();
	compat_cleanup ();
}

static void show_version ()
{
	printf (PACKAGE_STRING" ");

	/* Show build time */
#ifdef __DATE__
	printf ("Build: "__DATE__" ");
# ifdef __TIME__
	printf (__TIME__);
# endif
#endif
	
	putchar ('\n');

	/* Show compiled-in components */
	printf ("Compiled with:");
#ifdef HAVE_OSS
	printf (" OSS");
#endif
#ifdef HAVE_ALSA
	printf (" ALSA");
#endif
#ifdef HAVE_JACK
	printf (" JACK");
#endif
#ifndef NDEBUG
	printf (" DEBUG");
#endif
#ifdef HAVE_CURL
	printf (" internet streams");
#endif
#ifdef HAVE_SAMPLERATE
	printf (" resample");
#endif
	
	putchar ('\n');
	
}

/* Show program usage and exit */
static void show_usage (const char *prg_name) {
	printf (PACKAGE_STRING"\n"
"Usage:\n"
"%s [OPTIONS]... [FILE]...\n"
"-V --version           Print program version and exit.\n"
"-h --help              Print usage and exit.\n"
#ifndef NDEBUG
"-D --debug             Turn on logging to a file.\n"
#endif
"-S --server            Run only the server.\n"
"-F --foreground        Run server in foreground, log to stdout.\n"
"-R --sound-driver NAME Use the specified sound driver (oss, alsa, jack, null).\n"
"-m --music-dir         Start in MusicDir.\n"
"-a --append            Append the files/directories/playlists passed in\n"
"                       the command line to playlist and exit.\n"
"-c --clear             Clear the playlist and exit.\n"
"-p --play              Start playing from the first item on the playlist.\n"
"-l --playit            Play files given on command line without modifing the\n"
"                       playlist.\n"
"-s --stop              Stop playing.\n"
"-f --next              Play next song.\n"
"-r --previous          Play previous song.\n"
"-x --exit              Shutdown the server.\n"
"-T --theme theme       Use selected theme file (read from ~/.moc/themes if\n"
"                       the path is not absolute.\n"
"-C --config FILE       Use the specified config file instead of the default.\n"
"-M --moc-dir DIR       Use the specified MOC directory instead of the default.\n"
"-P --pause             Pause.\n"
"-U --unpause           Unpause.\n"
"-G --toggle-pause      Toggle between play/pause.\n"
"-v --volume (+/-)LEVEL Adjust PCM volume.\n"
"-y --sync              Synchronize the playlist with other clients.\n"
"-n --nosync            Don't synchronize the playlist with other clients.\n"
"-A --ascii             Use ASCII characters to draw lines.\n"
"-i --info              Print the information about the currently played file.\n"
"-e --recursively       Alias for -a.\n"
"-k --seek N            Seek by N seconds (can be negative).\n"
, prg_name);
}

/* Send commands requested in params to the server. */
static void server_command (struct parameters *params)
{
	int sock;

	if ((sock = server_connect()) == -1)
		fatal ("The server is not running");

	signal (SIGPIPE, SIG_IGN);
	if (ping_server(sock)) {
		if (params->exit) {
			if (!send_int(sock, CMD_QUIT))
				fatal ("Can't send command");
		}
		else if (params->stop) {
			if (!send_int(sock, CMD_STOP)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands");
		}
		else if (params->pause) {
			if (!send_int(sock, CMD_PAUSE)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands");
		}
		else if (params->next) {
			if (!send_int(sock, CMD_NEXT)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands");
		}
		else if (params->previous) {
			if (!send_int(sock, CMD_PREV)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands");
		}
		else if (params->unpause) {
			if (!send_int(sock, CMD_UNPAUSE)
					|| !send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands");
		}
		else if (params->toggle_pause) {
			int state;
			int ev;
			int cmd = -1;
			
			if (!send_int(sock, CMD_GET_STATE))
				fatal ("Can't send commands");
			if (!get_int(sock, &ev) || ev != EV_DATA
					|| !get_int(sock, &state))
				fatal ("Can't get data from the server");

			if (state == STATE_PAUSE)
				cmd = CMD_UNPAUSE;
			else if (state == STATE_PLAY)
				cmd = CMD_PAUSE;

			if (cmd != -1 && !send_int(sock, cmd))
				fatal ("Can't send commands");
			if (!send_int(sock, CMD_DISCONNECT))
				fatal ("Can't send commands");
		}
	}
	else
		fatal ("Can't connect to the server.");

	close (sock);
}

static long get_num_param (const char *p)
{
	char *e;
	long val;

	val = strtol (p, &e, 10);
	if (*e)
		fatal ("The parameter should be a number.");

	return val;
}

int main (int argc, char *argv[])
{
	struct option long_options[] = {
		{ "version",		0, NULL, 'V' },
		{ "help",		0, NULL, 'h' },
#ifndef NDEBUG
		{ "debug",		0, NULL, 'D' },
#endif
		{ "server",		0, NULL, 'S' },
		{ "foreground",		0, NULL, 'F' },
		{ "sound-driver",	1, NULL, 'R' },
		{ "music-dir",		0, NULL, 'm' },
		{ "append",		0, NULL, 'a' },
		{ "clear", 		0, NULL, 'c' },
		{ "play", 		0, NULL, 'p' },
		{ "playit",		0, NULL, 'l' },
		{ "stop",		0, NULL, 's' },
		{ "next",		0, NULL, 'f' },
		{ "previous",		0, NULL, 'r' },
		{ "exit",		0, NULL, 'x' },
		{ "theme",		1, NULL, 'T' },
		{ "config",		1, NULL, 'C' },
		{ "moc-dir",		1, NULL, 'M' },
		{ "pause",		0, NULL, 'P' },
		{ "unpause",		0, NULL, 'U' },
		{ "toggle-pause",	0, NULL, 'G' },
		{ "sync",		0, NULL, 'y' },
		{ "nosync",		0, NULL, 'n' },
		{ "ascii",		0, NULL, 'A' },
		{ "info",		0, NULL, 'i' },
		{ "recursively",	0, NULL, 'e' },
		{ "seek",		1, NULL, 'k' },
		{ "volume",		1, NULL, 'v' },
		{ 0, 0, 0, 0 }
	};
	int ret, opt_index = 0;
	struct parameters params;
	char *config_file = NULL;

	memset (&params, 0, sizeof(params));
	options_init ();

	while ((ret = getopt_long(argc, argv,
					"VhDSFR:macpsxT:C:M:PUynArfiGelk:v:",
					long_options, &opt_index)) != -1) {
		switch (ret) {
			case 'V':
				show_version ();
				return 0;
			case 'h':
				show_usage (argv[0]);
				return 0;
#ifndef NDEBUG
			case 'D':
				params.debug = 1;
				break;
#endif
			case 'S':
				params.only_server = 1;
				break;
			case 'F':
				params.foreground = 1;
				break;
			case 'R':
				if (!check_str_option("SoundDriver", optarg))
					fatal ("No such sound driver");
				option_set_str ("SoundDriver", optarg);
				option_ignore_config ("SoundDriver");
				break;
			case 'm':
				option_set_int ("StartInMusicDir", 1);
				option_ignore_config ("StartInMusicDir");
				break;
			case 'a':
			case 'e':
				params.append = 1;
				params.dont_run_iface = 1;
				break;
			case 'c':
				params.clear = 1;
				params.dont_run_iface = 1;
				break;
			case 'i':
				params.get_file_info = 1;
				params.dont_run_iface = 1;
				break;
			case 'p':
				params.play = 1;
				params.dont_run_iface = 1;
				break;
			case 'l':
				params.playit = 1;
				params.dont_run_iface = 1;
				break;
			case 's':
				params.stop = 1;
				params.dont_run_server = 1;
				break;
			case 'f':
				params.next = 1;
				params.dont_run_server = 1;
				break;
			case 'r':
				params.previous = 1;
				params.dont_run_server = 1;
				break;
			case 'x':
				params.exit = 1;
				params.dont_run_server = 1;
				break;
			case 'P':
				params.pause = 1;
				params.dont_run_server = 1;
				break;
			case 'U':
				params.unpause = 1;
				params.dont_run_server = 1;
				break;
			case 'T':
				option_set_str ("ForceTheme", optarg);
				break;
			case 'C':
				config_file = xstrdup (optarg);
				break;
			case 'M':
				option_set_str ("MOCDir", optarg);
				option_ignore_config ("MOCDir");
				break;
			case 'y':
				option_set_int ("SyncPlaylist", 1);
				option_ignore_config ("SyncPlaylist");
				break;
			case 'n':
				option_set_int ("SyncPlaylist", 0);
				option_ignore_config ("SyncPlaylist");
				break;
			case 'A':
				option_set_int ("ASCIILines", 1);
				option_ignore_config ("ASCIILines");
				break;
			case 'G':
				params.toggle_pause = 1;
				params.dont_run_server = 1;
				break;
			case 'k':
				params.seek_by = get_num_param (optarg);
				params.dont_run_iface = 1;
				break;
			case 'v' :
				params.adj_volume = optarg;
				params.dont_run_iface = 1;
				break;
			default:
				show_usage (argv[0]);
				return 1;
		}
	}
	
	if (params.foreground && !params.only_server)
		fatal ("Can't use --foreground without --server");
	if (params.dont_run_iface && params.only_server)
		fatal ("-c, -a and -p options can't be used with --server");

	options_parse (config_file ? config_file
			: create_file_name("config"));
	if (config_file)
		free (config_file);
	check_moc_dir ();
	
	if (params.dont_run_server)
		server_command (&params);
	else
		start_moc (&params, argv + optind, argc - optind);

	return 0;
}
