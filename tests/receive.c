/* A RELP receiver for testing.
 *
 * Copyright 2014 Mathias Nyman
 * Copyright 2018 Adiscon GmbH
 *
 * See getopt() call below for command line options. There is a brief
 * (buf hopefully sufficient) comment describing what each option does.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include "librelp.h"

#define TRY(f) if(f != RELP_RET_OK) { fprintf(stderr, "receive: FAILURE in '%s'\n", #f); ret = 1; goto done; }

static FILE *errFile = NULL;
static FILE *outFile = NULL;
static char *pidFileName = NULL;

static int immediate_exit = 0; /* if set to 1, force-exit as soon as possible */

static relpEngine_t *pRelpEngine;

#define USR_MAGIC 0x1234FFee
struct usrdata { /* used for testing user pointer pass-back */
	int magic;
	char *progname;
};
struct usrdata *userdata = NULL;
static void
hdlr_enable(int sig, void (*hdlr)())
{
	struct sigaction sigAct;
	memset(&sigAct, 0, sizeof (sigAct));
	sigemptyset(&sigAct.sa_mask);
	sigAct.sa_handler = hdlr;
	sigaction(sig, &sigAct, NULL);
}

void
terminate(__attribute__((unused)) const int sig)
{
	relpEngineSetStop(pRelpEngine);
}

/* handler to unconditionally exit the code - required for test where
 * server must "suddenly" abort.
 */
void
do_exit(__attribute__((unused)) const int sig)
{
	immediate_exit = 1;
}


static void __attribute__((format(printf, 1, 2)))
dbgprintf(char *fmt, ...)
{
	va_list ap;
	char pszWriteBuf[32*1024+1];

	va_start(ap, fmt);
	vsnprintf(pszWriteBuf, sizeof(pszWriteBuf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "receive: %s", pszWriteBuf);
	fflush(stderr);
}

static relpRetVal onSyslogRcv(unsigned char *pHostname __attribute__((unused)),
	unsigned char *pIP __attribute__((unused)), unsigned char *msg,
	size_t lenMsg)
{

	char *pMsg;

	pMsg = (char *) malloc((int)lenMsg+1);
	memset(pMsg, '\0', lenMsg+1);
	memcpy(pMsg, msg, lenMsg);

	fprintf(outFile, "%s\n", pMsg);
	fflush(outFile);

	free(pMsg);

	if(immediate_exit) {
		fprintf(stderr, "receive: force-exit %lld by user request\n", (long long) getpid());
		exit(1);
	}

	return RELP_RET_OK;
}

void print_usage(void)
{
	printf("Usage: ./receive -p <PORTNUM>\n");
}

static void
onErr(void *pUsr, char *objinfo, char* errmesg, __attribute__((unused)) relpRetVal errcode)
{
	struct usrdata *pThis = (struct usrdata*) pUsr;
	if(pUsr != userdata) {
		fprintf(stderr, "receive: pUsr NOT pointing to usrdata!\n");
	}
	if(pThis->magic != USR_MAGIC) {
		fprintf(stderr, "receive: pUsr magic incorrect in onErr, magic %8.8x "
			"pUsr %p\n", pThis->magic, (void*) pThis);
	}
	fprintf(stderr, "%s: error '%s', object '%s'\n", pThis->progname, errmesg, objinfo);
	if(errFile != NULL) {
		fprintf(errFile, "receive: error '%s', object '%s'\n", errmesg, objinfo);
	}
}

static void
onGenericErr(char *objinfo, char* errmesg, __attribute__((unused)) relpRetVal errcode)
{
	fprintf(stderr, "receive: librelp error '%s', object '%s'\n", errmesg, objinfo);
	if(errFile != NULL) {
		fprintf(errFile, "receive: librelp error '%s', object '%s'\n", errmesg, objinfo);
	}
}

static void
onAuthErr( __attribute__((unused)) void *pUsr, char *authinfo,
	char* errmesg, __attribute__((unused)) relpRetVal errcode)
{
	fprintf(stderr, "receive: authentication error '%s', object '%s'\n", errmesg, authinfo);
	if(errFile != NULL) {
		fprintf(errFile, "receive: authentication error '%s', object '%s'\n", errmesg, authinfo);
	}
}

static void
exit_hdlr(void)
{
	if(userdata != NULL) {
		free(userdata->progname);
		free(userdata);
	}
	if(errFile != NULL) {
		fclose(errFile);
	}
	if(outFile != NULL) {
		fclose(outFile);
	}
	if(pidFileName != NULL) {
		unlink(pidFileName);
	}

}

int main(int argc, char *argv[]) {

	int c;
	int option_index = 0;
	unsigned char *port = NULL;
	int verbose = 0;
	int protFamily = 2; /* IPv4=2, IPv6=10 */
	relpSrv_t *pRelpSrv;
	int bEnableTLS = 0;
	char *caCertFile = NULL;
	char *myCertFile = NULL;
	char *myPrivKeyFile = NULL;
	char *permittedPeer = NULL;
	char *authMode = NULL;
	int maxDataSize = 0;
	int oversizeMode = 0;
	int ret = 0;
	int append_outfile = 0;
	const char* outfile_name = NULL;

	static struct option long_options[] =
	{
		{"ca", required_argument, 0, 'x'},
		{"cert", required_argument, 0, 'y'},
		{"key", required_argument, 0, 'z'},
		{"peer", required_argument, 0, 'P'},
		{"authmode", required_argument, 0, 'a'},
		{"pidfile", required_argument, 0, 'F'},
		{"errorfile", required_argument, 0, 'e'},
		{"outfile", required_argument, 0, 'O'},
		{"append-outfile", no_argument, 0, 'A'},
		{0, 0, 0, 0}
	};


	while((c = getopt_long(argc, argv, "a:Ae:F:m:o:O:P:p:Tvx:y:z:", long_options, &option_index)) != -1) {
		switch(c) {
		case 'a':
			authMode = optarg;
			break;
		case 'A':
			append_outfile = 1;
			break;
		case 'e':
			if((errFile = fopen((char*) optarg, "w")) == NULL) {
				perror(optarg);
				fprintf(stderr, "receive: error opening error file\n");
				exit(1);
			}
			break;
		case 'v':
			verbose = 1;
			break;
		case 'F': /* pid file name */
			pidFileName = optarg;
			break;
		case 'm': /* message size */
			maxDataSize = atoi(optarg);
			if(maxDataSize < 128) {
				printf("maxMessageSize tried to set to %d, "
					"but cannot be less than 128 - set "
					"to 128 instead\n", maxDataSize);
				maxDataSize = 128;
			} else if(maxDataSize > INT_MAX) {
				printf("maxMessageSize tried to set to %d, "
					"but cannot be more than INT_MAX - set "
					"to INT_MAX instead\n", maxDataSize);
				maxDataSize = INT_MAX;
			}
			break;
		case 'O': /* output file */
			outfile_name = optarg;
			break;
		case 'o': /* oversize mode */
			if(strcmp("truncate", optarg) == 0) {
				oversizeMode = RELP_OVERSIZE_TRUNCATE;
			} else if(strcmp("abort", optarg) == 0) {
				oversizeMode = RELP_OVERSIZE_ABORT;
			} else if(strcmp("accept", optarg) == 0) {
				oversizeMode = RELP_OVERSIZE_ACCEPT;
			} else {
				printf("Wrong oversizeMode, default used.\n");
			}
			break;
		case 'P':
			permittedPeer = optarg;
			break;
		case 'p':
			port = (unsigned char*)optarg;
			break;
		case 'T':
			bEnableTLS = 1;
			break;
		case 'x':
			caCertFile = optarg;
			break;
		case 'y':
			myCertFile = optarg;
			break;
		case 'z':
			myPrivKeyFile = optarg;
			break;
		default:
			print_usage();
			return -1;
		}
	}

	atexit(exit_hdlr);

	if(outFile == NULL) {
		outFile = stdout;
	}

	if(port == NULL) {
		printf("Port is missing\n");
		print_usage();
		goto done;
	}

	if(authMode != NULL) {
		if(	(strcasecmp(authMode, "certvalid") != 0 && permittedPeer == NULL) ||
			caCertFile == NULL || myCertFile == NULL || myPrivKeyFile == NULL) {
			printf("receive: mode '%s' parameter missing; certificates and permittedPeer required\n",
				authMode);
			goto done;
		}
	}



	if(caCertFile != NULL || myCertFile != NULL || myPrivKeyFile != NULL) {
		if(bEnableTLS == 0) {
			printf("receive: Certificates were specified, but TLS was "
			       "not enabled! Will continue without TLS. To enable "
			       "it use parameter \"-T\"\n");
			goto done;
		}
	}

	hdlr_enable(SIGTERM, terminate);
	hdlr_enable(SIGUSR1, do_exit);

	if(outfile_name != NULL) {
		if((outFile = fopen(outfile_name, append_outfile ? "a" : "w")) == NULL) {
			perror(outfile_name);
			fprintf(stderr, "receive: error opening output file\n");
			exit(1);
		}
	}

	TRY(relpEngineConstruct(&pRelpEngine));
	TRY(relpEngineSetDbgprint(pRelpEngine, verbose ? dbgprintf : NULL));
	TRY(relpEngineSetEnableCmd(pRelpEngine, (unsigned char*) "syslog", eRelpCmdState_Required));
	TRY(relpEngineSetFamily(pRelpEngine, protFamily));
	TRY(relpEngineSetSyslogRcv(pRelpEngine, onSyslogRcv));

	TRY(relpEngineSetOnErr(pRelpEngine, onErr));
	TRY(relpEngineSetOnGenericErr(pRelpEngine, onGenericErr));
	TRY(relpEngineSetOnAuthErr(pRelpEngine, onAuthErr));

	TRY(relpEngineSetDnsLookupMode(pRelpEngine, 0)); /* 0=disable */

	TRY(relpEngineListnerConstruct(pRelpEngine, &pRelpSrv));
	TRY(relpSrvSetLstnPort(pRelpSrv, port));
	userdata = calloc(1, sizeof(struct usrdata));
	userdata->magic = USR_MAGIC;
	userdata->progname = strdup("receive");
	relpSrvSetUsrPtr(pRelpSrv, userdata);
	if(maxDataSize != 0) {
		TRY(relpSrvSetMaxDataSize(pRelpSrv, maxDataSize));
	}
	if(oversizeMode != 0) {
		TRY(relpSrvSetOversizeMode(pRelpSrv, oversizeMode));
	}

	if(bEnableTLS) {
		TRY(relpSrvEnableTLS2(pRelpSrv));
		if(authMode != NULL) {
			TRY(relpSrvSetAuthMode(pRelpSrv, authMode));
			TRY(relpSrvSetCACert(pRelpSrv, caCertFile));
			TRY(relpSrvSetOwnCert(pRelpSrv, myCertFile));
			TRY(relpSrvSetPrivKey(pRelpSrv, myPrivKeyFile));
			if (permittedPeer != NULL) {
				TRY(relpSrvAddPermittedPeer(pRelpSrv, permittedPeer));
			}
		}
	}

	TRY(relpEngineListnerConstructFinalize(pRelpEngine, pRelpSrv));

	if(pidFileName != NULL) {
		FILE *fp;
		if((fp = fopen((char*) pidFileName, "w")) == NULL) {
			fprintf(stderr, "receive: couldn't open PidFile\n");
			if(errFile != NULL) {
				fprintf(errFile, "receive: couldn't open PidFile\n");
			}
			ret = 1;
			goto done;
		}
		if(fprintf(fp, "%d", getpid()) < 0) {
			fprintf(stderr, "receive: couldn't write to PidFile\n");
			if(errFile != NULL) {
				fprintf(errFile, "receive: couldn't write to PidFile\n");
			}
			ret = 1;
			goto done;
		}
		fclose(fp);
	}

	int i = 0;
	while(relpEngineRun(pRelpEngine) != RELP_RET_OK) {
		fprintf(stderr, "receive: error starting relp engine, try %d\n", i);
		++i;
		if(i >= 10) {
			fprintf(stderr, "receive: giving up starting relp engine\n");
			break;
		}
		sleep(1);
	}

	TRY(relpEngineDestruct(&pRelpEngine));

done:
	return ret;
}
