/*
	Copyright (C) 2006 by Jonas Kramer
	Published under the terms of the GNU General Public License (GPL).
*/

#define _GNU_SOURCE


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <stdarg.h>

#include "hash.h"
#include "http.h"
#include "play.h"
#include "settings.h"
#include "md5.h"
#include "history.h"
#include "service.h"
#include "playlist.h"
#include "ropen.h"
#include "strary.h"
#include "sckif.h"

#include "globals.h"

struct hash data; /* Warning! MUST be bzero'd ASAP or we're all gonna die! */

pid_t playfork = 0; /* PID of the decoding & playing process, if running */

struct playlist playlist;
char * currentStation = NULL;


int authenticate(const char * username, const char * password) {
	const unsigned char * md5;
	char hexmd5[32 + 1] = { 0 }, url[512] = { 0 }, ** response;
	char * encuser = NULL;
	unsigned ndigit, i = 0;
	const char * session, * fmt =
		"http://ws.audioscrobbler.com/radio/handshake.php"
		"?version=0.1"
		"&platform=linux"
		"&username=%s"
		"&passwordmd5=%s"
		"&debug=0"
		"&language=en";

	memset(& data, 0, sizeof(struct hash));

	/* create the hash, then convert to ASCII */
	md5 = MD5((const unsigned char *) password, strlen(password));
	for(ndigit = 0; ndigit < 16; ++ndigit)
		sprintf(2 * ndigit + hexmd5, "%02x", md5[ndigit]);

	set(& rc, "password", hexmd5);

	/* escape username for URL */
	encode(username, & encuser);

	/* put handshake URL together and fetch initial data from server */
	snprintf(url, sizeof(url), fmt, encuser, hexmd5);
	free(encuser);

	response = fetch(url, NULL, NULL, NULL);
	if(!response) {
		fputs("No response.\n", stderr);
		return 0;
	}

	while(response[i]) {
		char key[64] = { 0 }, val[256] = { 0 };
		sscanf(response[i], "%63[^=]=%255[^\r\n]", key, val);
		set(& data, key, val);
		free(response[i++]);
	}
	free(response);

	session = value(& data, "session");
	if(!session || !strcmp(session, "FAILED")) {
		fputs("Authentication failed.\n", stderr);
		unset(& data, "session");
		return 0;
	}

	return !0;
}


int station(const char * stationURL) {
	char url[512] = { 0 }, * encodedURL = NULL, ** response;
	unsigned i = 0, retval = !0, regular = !0;
	const char * fmt;	
	const char * types[4] = {"play", "preview", "track", "playlist"};

	delayquit = 0;

	if(playfork && haskey(& rc, "delay-change")) {
		if(nextstation) {
			/*
			  Cancel station change if url is empty or equal to the current
			  station.
			*/
			free(nextstation);
			nextstation = NULL;

			if(!strlen(stationURL) || !strcmp(stationURL, currentStation)) {
				puts("Station change cancelled.");
				return 0;
			}
		}

		puts("\rDelayed.");
		nextstation = strdup(stationURL);

		return 0;
	}

	freelist(& playlist);

	if(!haskey(& data, "session")) {
		fputs("Not authenticated, yet.\n", stderr);
		return 0;
	}

	if(!stationURL)
		return 0;

	if(!strncasecmp(stationURL, "lastfm://", 9))
		stationURL += 9;

	/* Check if it's a static playlist of tracks or track previews. */
	for(i = 0; i < 4; ++i)
		if(!strncasecmp(types[i], stationURL, strlen(types[i]))) {
			regular = 0;
			break;
		}

	/*
	   If this is not a special "one-time" stream, it's a regular radio
	   station and we request it using the good old /adjust.php URL.
	   If it's not a regular stream, the reply of this request already is
	   a XSPF playlist we have to parse.
   */
	if(regular) {
		fmt = "http://ws.audioscrobbler.com/radio/adjust.php?session=%s&url=%s";
	}
	else {
		fmt =
			"http://ws.audioscrobbler.com/1.0/webclient/getresourceplaylist.php"
			"?sk=%s&url=%s&desktop=1";
	}

	encode(stationURL, & encodedURL);
	snprintf(url, sizeof(url), fmt, value(& data, "session"), encodedURL);
	free(encodedURL);

	if(!(response = fetch(url, NULL, NULL, NULL)))
		return 0;

	if(regular) {
		for(i = 0; response[i]; ++i) {
			char status[64] = { 0 };
			if(sscanf(response[i], "response=%63[^\r\n]", status) > 0)
				if(!strncmp(status, "FAILED", 6))
					retval = 0;
		}

		purge(response);
		response = NULL;

		if(!retval) {
			printf("Sorry, couldn't set station to %s.\n", stationURL);
			return 0;
		}

		expand(& playlist);
	}
	else {
		char * xml = join(response, 0);

		response = NULL;

		freelist(& playlist);

		if(!parsexspf(& playlist, xml))
			retval = 0;

		free(xml);
	}

	enable(CHANGED);
	histapp(stationURL);

	if(currentStation)
		free(currentStation);

	currentStation = strdup(stationURL);
	assert(currentStation != NULL);

	if(retval && playfork) {
		enable(INTERRUPTED);
		kill(playfork, SIGUSR1);
	}

	return retval;
}


/*
	Takes pointer to a playlist, forks off a playback process and tries to play
	the next track in the playlist. If there's already a playback process, it's
	killed first (which means the currently played track is skipped).
*/
int play(struct playlist * list) {
	unsigned i;
	char * keys [] = {
		"creator", "title", "album", "duration", "station",
		"lastfm:trackauth", "trackpage", "artistpage", "albumpage",
		"image", "freeTrackURL",
	};

	assert(list != NULL);

	if(!list->left)
		return 0;

	if(playfork) {
		kill(playfork, SIGUSR1);
		return !0;
	}

	enable(QUIET);

	empty(& track);

	for(i = 0; i < (sizeof(keys) / sizeof(char *)); ++i)
		set(& track, keys[i], value(& list->track->track, keys[i]));

	playfork = fork();

	if(!playfork) {
		FILE * fd = NULL;
		const char * location = value(& list->track->track, "location");

		rmsckif();

		subfork = 0;

		if(location != NULL) {
			fetch(location, & fd, NULL, NULL);

			if(fd != NULL) {
				if(!playback(fd))
					kill(getppid(), SIGUSR2);
				fshutdown(& fd);
			}
		}

		exit(EXIT_SUCCESS);
	}

	return !0;
}
