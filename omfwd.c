/* omfwd.c
 * This is the implementation of the build-in forwarding output module.
 *
 * NOTE: read comments in module-template.h to understand how this file
 *       works!
 *
 * File begun on 2007-07-20 by RGerhards (extracted from syslogd.c)
 * This file is under development and has not yet arrived at being fully
 * self-contained and a real object. So far, it is mostly an excerpt
 * of the "old" message code without any modifications. However, it
 * helps to have things at the right place one we go to the meat of it.
 *
 * Copyright 2007 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h"
#ifdef SYSLOG_INET
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fnmatch.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#ifdef USE_NETZIP
#include <zlib.h>
#endif
#include <pthread.h>
#include "syslogd.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "net.h"
#include "omfwd.h"
#include "template.h"
#include "msg.h"
#include "tcpsyslog.h"
#include "cfsysline.h"
#include "module-template.h"

MODULE_TYPE_OUTPUT

#ifdef SYSLOG_INET
#define INET_SUSPEND_TIME 60		/* equal to 1 minute 
					 * rgerhards, 2005-07-26: This was 3 minutes. As the
					 * same timer is used for tcp based syslog, we have
					 * reduced it. However, it might actually be worth
					 * thinking about a buffered tcp sender, which would be 
					 * a much better alternative. When that happens, this
					 * time here can be re-adjusted to 3 minutes (or,
					 * even better, made configurable).
					 */
#define INET_RETRY_MAX 30		/* maximum of retries for gethostbyname() */
	/* was 10, changed to 30 because we reduced INET_SUSPEND_TIME by one third. So
	 * this "fixes" some of implications of it (see comment on INET_SUSPEND_TIME).
	 * rgerhards, 2005-07-26
	 */
#endif

/* internal structures
 */
DEF_OMOD_STATIC_DATA

typedef struct _instanceData {
	char	f_hname[MAXHOSTNAMELEN+1];
	short	sock;			/* file descriptor */
	int *pSockArray;		/* sockets to use for UDP */
	enum { /* TODO: we shoud revisit these definitions */
		eDestFORW,
		eDestFORW_SUSP,
		eDestFORW_UNKN
	} eDestState;
	int iRtryCnt;
	struct addrinfo *f_addr;
	int compressionLevel; /* 0 - no compression, else level for zlib */
	char *port;
	int protocol;
	TCPFRAMINGMODE tcp_framing;
#	define	FORW_UDP 0
#	define	FORW_TCP 1
	/* following fields for TCP-based delivery */
	time_t	ttSuspend;	/* time selector was suspended */
} instanceData;

/* get the syslog forward port from selector_t. The passed in
 * struct must be one that is setup for forwarding.
 * rgerhards, 2007-06-28
 * We may change the implementation to try to lookup the port
 * if it is unspecified. So far, we use the IANA default auf 514.
 */
char *getFwdSyslogPt(instanceData *pData)
{
	assert(pData != NULL);
	if(pData->port == NULL)
		return("514");
	else
		return(pData->port);
}

BEGINcreateInstance
CODESTARTcreateInstance
	pData->sock = -1;
ENDcreateInstance


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINfreeInstance
CODESTARTfreeInstance
	switch (pData->eDestState) {
		case eDestFORW:
		case eDestFORW_SUSP:
			freeaddrinfo(pData->f_addr);
			/* fall through */
		case eDestFORW_UNKN:
			if(pData->port != NULL)
				free(pData->port);
			break;
	}

	/* final cleanup */
	if(pData->sock >= 0)
		close(pData->sock);
	if(pData->pSockArray != NULL)
		closeUDPListenSockets(pData->pSockArray);
ENDfreeInstance


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	printf("%s", pData->f_hname);
ENDdbgPrintInstInfo


/* Send a message via UDP
 * rgehards, 2007-12-20
 */
static rsRetVal UDPSend(instanceData *pData, char *msg, size_t len)
{
	DEFiRet;
	struct addrinfo *r;
	int i;
	unsigned lsent = 0;
	int bSendSuccess;

	if(pData->pSockArray != NULL) {
		/* we need to track if we have success sending to the remote
		 * peer. Success is indicated by at least one sendto() call
		 * succeeding. We track this be bSendSuccess. We can not simply
		 * rely on lsent, as a call might initially work, but a later
		 * call fails. Then, lsent has the error status, even though
		 * the sendto() succeeded.
		 * rgerhards, 2007-06-22
		 */
		bSendSuccess = FALSE;
		for (r = pData->f_addr; r; r = r->ai_next) {
			for (i = 0; i < *pData->pSockArray; i++) {
			       lsent = sendto(pData->pSockArray[i+1], msg, len, 0, r->ai_addr, r->ai_addrlen);
				if (lsent == len) {
					bSendSuccess = TRUE;
					break;
				} else {
					int eno = errno;
					char errStr[1024];
					dbgprintf("sendto() error: %d = %s.\n",
						eno, rs_strerror_r(eno, errStr, sizeof(errStr)));
				}
			}
			if (lsent == len && !send_to_all)
			       break;
		}
		/* finished looping */
		if (bSendSuccess == FALSE) {
			dbgprintf("error forwarding via udp, suspending\n");
			iRet = RS_RET_SUSPENDED;
		}
	}

	RETiRet;
}

/* CODE FOR SENDING TCP MESSAGES */


/* Send a frame via plain TCP protocol
 * rgerhards, 2007-12-28
 */
static rsRetVal TCPSendFrame(void *pvData, char *msg, size_t len)
{
	DEFiRet;
	ssize_t lenSend;
	instanceData *pData = (instanceData *) pvData;

	lenSend = send(pData->sock, msg, len, 0);
	dbgprintf("TCP sent %ld bytes, requested %ld\n", (long) lenSend, (long) len);

	if(lenSend == -1) {
		/* we have an error case - check what we can live with */
		switch(errno) {
		case EMSGSIZE:
			dbgprintf("message not (tcp)send, too large\n");
			/* This is not a real error, so it is not flagged as one */
			break;
		default:
			dbgprintf("message not (tcp)send");
			iRet = RS_RET_TCP_SEND_ERROR;
			break;
		}
	} else if(lenSend != (ssize_t) len) {
		/* no real error, could "just" not send everything... 
		 * For the time being, we ignore this...
		 * rgerhards, 2005-10-25
		 */
		dbgprintf("message not completely (tcp)send, ignoring %ld\n", lenSend);
		usleep(1000); /* experimental - might be benefitial in this situation */
		/* TODO: we need to revisit this code -- rgerhards, 2007-12-28 */
	}

	RETiRet;
}


/* This function is called immediately before a send retry is attempted.
 * It shall clean up whatever makes sense.
 * rgerhards, 2007-12-28
 */
static rsRetVal TCPSendPrepRetry(void *pvData)
{
	instanceData *pData = (instanceData *) pvData;

	assert(pData != NULL);
	close(pData->sock);
	pData->sock = -1;
	return RS_RET_OK;
}


/* initialies everything so that TCPSend can work.
 * rgerhards, 2007-12-28
 */
static rsRetVal TCPSendInit(void *pvData)
{
	DEFiRet;
	instanceData *pData = (instanceData *) pvData;

	assert(pData != NULL);
	if(pData->sock < 0) {
		if((pData->sock = TCPSendCreateSocket(pData->f_addr)) < 0)
			iRet = RS_RET_TCP_SOCKCREATE_ERR;
	}

	RETiRet;
}


/* try to resume connection if it is not ready
 * rgerhards, 2007-08-02
 */
static rsRetVal doTryResume(instanceData *pData)
{
	DEFiRet;
	struct addrinfo *res;
	struct addrinfo hints;
	unsigned e;

	switch (pData->eDestState) {
	case eDestFORW_SUSP:
		iRet = RS_RET_OK; /* the actual check happens during doAction() only */
		pData->eDestState = eDestFORW;
		break;
		
	case eDestFORW_UNKN:
		/* The remote address is not yet known and needs to be obtained */
		dbgprintf(" %s\n", pData->f_hname);
		memset(&hints, 0, sizeof(hints));
		/* port must be numeric, because config file syntax requests this */
		/* TODO: this code is a duplicate from cfline() - we should later create
		 * a common function.
		 */
		hints.ai_flags = AI_NUMERICSERV;
		hints.ai_family = family;
		hints.ai_socktype = pData->protocol == FORW_UDP ? SOCK_DGRAM : SOCK_STREAM;
		if((e = getaddrinfo(pData->f_hname,
				    getFwdSyslogPt(pData), &hints, &res)) == 0) {
			dbgprintf("%s found, resuming.\n", pData->f_hname);
			pData->f_addr = res;
			pData->iRtryCnt = 0;
			pData->eDestState = eDestFORW;
		} else {
			iRet = RS_RET_SUSPENDED;
		}
		break;
	case eDestFORW:
		/* rgerhards, 2007-09-11: this can not happen, but I've included it to
		 * a) make the compiler happy, b) detect any logic errors */
		assert(0);
		break;
	}

	RETiRet;
}


BEGINtryResume
CODESTARTtryResume
	iRet = doTryResume(pData);
ENDtryResume

BEGINdoAction
	char *psz; /* temporary buffering */
	register unsigned l;
CODESTARTdoAction
	switch (pData->eDestState) {
	case eDestFORW_SUSP:
		dbgprintf("internal error in omfwd.c, eDestFORW_SUSP in doAction()!\n");
		iRet = RS_RET_SUSPENDED;
		break;
		
	case eDestFORW_UNKN:
		dbgprintf("doAction eDestFORW_UNKN\n");
		iRet = doTryResume(pData);
		break;

	case eDestFORW:
		dbgprintf(" %s:%s/%s\n", pData->f_hname, getFwdSyslogPt(pData),
			 pData->protocol == FORW_UDP ? "udp" : "tcp");
		/* with UDP, check if the socket is there and, if not, alloc
 		 * it. TODO: there should be a better place for that code.
 		 * rgerhards, 2007-12-26
 		 */
		if(pData->protocol == FORW_UDP) {
			if(pData->pSockArray == NULL) {
				pData->pSockArray = create_udp_socket((uchar*)pData->f_hname, NULL, 0);
			}
		}
		if ( 0) // TODO: think about this strcmp(getHOSTNAME(f->f_pMsg), LocalHostName) && NoHops )
		/* what we need to do is get the hostname as an additonal string (during parseSe..). Then,
		 * we can compare that string to LocalHostName. That way, we do not need to access the
		 * msgobject, and everything is clean. The question remains, though, if that functionality
		 * here actually makes sense or not. If we really need it, it might make more sense to compare
		 * the target IP address to the IP addresses of the local machene - that is a far better way of
		 * handling things than to relay on the error-prone hostname property.
		 * rgerhards, 2007-07-27
		 */
			dbgprintf("Not sending message to remote.\n");
		else {
			pData->ttSuspend = time(NULL);
			psz = (char*) ppString[0];
			l = strlen((char*) psz);
			if (l > MAXLINE)
				l = MAXLINE;

#			ifdef	USE_NETZIP
			/* Check if we should compress and, if so, do it. We also
			 * check if the message is large enough to justify compression.
			 * The smaller the message, the less likely is a gain in compression.
			 * To save CPU cycles, we do not try to compress very small messages.
			 * What "very small" means needs to be configured. Currently, it is
			 * hard-coded but this may be changed to a config parameter.
			 * rgerhards, 2006-11-30
			 */
			if(pData->compressionLevel && (l > MIN_SIZE_FOR_COMPRESS)) {
				Bytef out[MAXLINE+MAXLINE/100+12] = "z";
				uLongf destLen = sizeof(out) / sizeof(Bytef);
				uLong srcLen = l;
				int ret;
				ret = compress2((Bytef*) out+1, &destLen, (Bytef*) psz,
						srcLen, pData->compressionLevel);
				dbgprintf("Compressing message, length was %d now %d, return state  %d.\n",
					l, (int) destLen, ret);
				if(ret != Z_OK) {
					/* if we fail, we complain, but only in debug mode
					 * Otherwise, we are silent. In any case, we ignore the
					 * failed compression and just sent the uncompressed
					 * data, which is still valid. So this is probably the
					 * best course of action.
					 * rgerhards, 2006-11-30
					 */
					dbgprintf("Compression failed, sending uncompressed message\n");
				} else if(destLen+1 < l) {
					/* only use compression if there is a gain in using it! */
					dbgprintf("there is gain in compression, so we do it\n");
					psz = (char*) out;
					l = destLen + 1; /* take care for the "z" at message start! */
				}
				++destLen;
			}
#			endif

			if(pData->protocol == FORW_UDP) {
				/* forward via UDP */
				CHKiRet(UDPSend(pData, psz, l));
			} else {
				/* forward via TCP */
				rsRetVal ret;
				ret = TCPSend(pData, psz, l, pData->tcp_framing, TCPSendInit, TCPSendFrame, TCPSendPrepRetry);
				if(ret != RS_RET_OK) {
					/* error! */
					dbgprintf("error forwarding via tcp, suspending\n");
					pData->eDestState = eDestFORW_SUSP;
					iRet = RS_RET_SUSPENDED;
				}
			}
		}
		break;
	}
finalize_it:
ENDdoAction


BEGINparseSelectorAct
	uchar *q;
	int i;
        int error;
	int bErr;
        struct addrinfo hints, *res;
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(1)
	if(*p == '@') {
		if((iRet = createInstance(&pData)) != RS_RET_OK)
			goto finalize_it;
		++p; /* eat '@' */
		if(*p == '@') { /* indicator for TCP! */
			pData->protocol = FORW_TCP;
			++p; /* eat this '@', too */
		} else {
			pData->protocol = FORW_UDP;
		}
		/* we are now after the protocol indicator. Now check if we should
		 * use compression. We begin to use a new option format for this:
		 * @(option,option)host:port
		 * The first option defined is "z[0..9]" where the digit indicates
		 * the compression level. If it is not given, 9 (best compression) is
		 * assumed. An example action statement might be:
		 * @@(z5,o)127.0.0.1:1400  
		 * Which means send via TCP with medium (5) compresion (z) to the local
		 * host on port 1400. The '0' option means that octet-couting (as in
		 * IETF I-D syslog-transport-tls) is to be used for framing (this option
		 * applies to TCP-based syslog only and is ignored when specified with UDP).
		 * That is not yet implemented.
		 * rgerhards, 2006-12-07
		 */
		if(*p == '(') {
			/* at this position, it *must* be an option indicator */
			do {
				++p; /* eat '(' or ',' (depending on when called) */
				/* check options */
				if(*p == 'z') { /* compression */
#					ifdef USE_NETZIP
					++p; /* eat */
					if(isdigit((int) *p)) {
						int iLevel;
						iLevel = *p - '0';
						++p; /* eat */
						pData->compressionLevel = iLevel;
					} else {
						logerrorInt("Invalid compression level '%c' specified in "
						         "forwardig action - NOT turning on compression.",
							 *p);
					}
#					else
					logerror("Compression requested, but rsyslogd is not compiled "
					         "with compression support - request ignored.");
#					endif /* #ifdef USE_NETZIP */
				} else if(*p == 'o') { /* octet-couting based TCP framing? */
					++p; /* eat */
					/* no further options settable */
					pData->tcp_framing = TCP_FRAMING_OCTET_COUNTING;
				} else { /* invalid option! Just skip it... */
					logerrorInt("Invalid option %c in forwarding action - ignoring.", *p);
					++p; /* eat invalid option */
				}
				/* the option processing is done. We now do a generic skip
				 * to either the next option or the end of the option
				 * block.
				 */
				while(*p && *p != ')' && *p != ',')
					++p;	/* just skip it */
			} while(*p && *p == ','); /* Attention: do.. while() */
			if(*p == ')')
				++p; /* eat terminator, on to next */
			else
				/* we probably have end of string - leave it for the rest
				 * of the code to handle it (but warn the user)
				 */
				logerror("Option block not terminated in forwarding action.");
		}
		/* extract the host first (we do a trick - we replace the ';' or ':' with a '\0')
		 * now skip to port and then template name. rgerhards 2005-07-06
		 */
		for(q = p ; *p && *p != ';' && *p != ':' ; ++p)
		 	/* JUST SKIP */;

		pData->port = NULL;
		if(*p == ':') { /* process port */
			uchar * tmp;

			*p = '\0'; /* trick to obtain hostname (later)! */
			tmp = ++p;
			for(i=0 ; *p && isdigit((int) *p) ; ++p, ++i)
				/* SKIP AND COUNT */;
			pData->port = malloc(i + 1);
			if(pData->port == NULL) {
				logerror("Could not get memory to store syslog forwarding port, "
					 "using default port, results may not be what you intend\n");
				/* we leave f_forw.port set to NULL, this is then handled by
				 * getFwdSyslogPt().
				 */
			} else {
				memcpy(pData->port, tmp, i);
				*(pData->port + i) = '\0';
			}
		}
		
		/* now skip to template */
		bErr = 0;
		while(*p && *p != ';') {
			if(*p && *p != ';' && !isspace((int) *p)) {
				if(bErr == 0) { /* only 1 error msg! */
					bErr = 1;
					errno = 0;
					logerror("invalid selector line (port), probably not doing "
					         "what was intended");
				}
			}
			++p;
		}
	
		/* TODO: make this if go away! */
		if(*p == ';') {
			*p = '\0'; /* trick to obtain hostname (later)! */
			strcpy(pData->f_hname, (char*) q);
			*p = ';';
		} else
			strcpy(pData->f_hname, (char*) q);

		/* process template */
		if((iRet = cflineParseTemplateName(&p, *ppOMSR, 0, OMSR_NO_RQD_TPL_OPTS, (uchar*) " StdFwdFmt"))
		   != RS_RET_OK)
			goto finalize_it;

		/* first set the pData->eDestState */
		memset(&hints, 0, sizeof(hints));
		/* port must be numeric, because config file syntax requests this */
		hints.ai_flags = AI_NUMERICSERV;
		hints.ai_family = family;
		hints.ai_socktype = pData->protocol == FORW_UDP ? SOCK_DGRAM : SOCK_STREAM;
		if( (error = getaddrinfo(pData->f_hname, getFwdSyslogPt(pData), &hints, &res)) != 0) {
			pData->eDestState = eDestFORW_UNKN;
			pData->iRtryCnt = INET_RETRY_MAX;
			pData->ttSuspend = time(NULL);
		} else {
			pData->eDestState = eDestFORW;
			pData->f_addr = res;
		}

		/*
		 * Otherwise the host might be unknown due to an
		 * inaccessible nameserver (perhaps on the same
		 * host). We try to get the ip number later, like
		 * FORW_SUSP.
		 */
	} else {
		iRet = RS_RET_CONFLINE_UNPROCESSED;
	}

	/* TODO: do we need to call freeInstance if we failed - this is a general question for
	 * all output modules. I'll address it lates as the interface evolves. rgerhards, 2007-07-25
	 */
CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINneedUDPSocket
CODESTARTneedUDPSocket
	iRet = RS_RET_TRUE;
ENDneedUDPSocket


BEGINmodExit
CODESTARTmodExit
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
ENDqueryEtryPt


BEGINmodInit(Fwd)
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
ENDmodInit

#endif /* #ifdef SYSLOG_INET */
/*
 * vi:set ai:
 */
