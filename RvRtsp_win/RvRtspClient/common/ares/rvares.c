/***********************************************************************
Filename   : rvares.c
Description: Interface functions for asynchronous DNS resolving services
************************************************************************
        Copyright (c) 2001 RADVISION Inc. and RADVISION Ltd.
************************************************************************
NOTICE:
This document contains information that is confidential and proprietary
to RADVISION Inc. and RADVISION Ltd.. No part of this document may be
reproduced in any form whatsoever without written prior approval by
RADVISION Inc. or RADVISION Ltd..

RADVISION Inc. and RADVISION Ltd. reserve the right to revise this
publication and make changes without obligation to notify any person of
such revisions or changes.
***********************************************************************/

#include "rvccore.h"
#include "rvlog.h"

#if (RV_DNS_TYPE == RV_DNS_ARES)

#include "rvansi.h"
#include "ares_dns.h"
#include <string.h>
#include "rvares.h"
#include "ares_private.h"
#include "rvthread.h"
#include "rvarescache.h"
#include "rvccoreglobals.h"
#include "rvstrutils.h"

/* RvCnameChain
 * 
 * Holds information about chain of CNAMEs in response (needed for NXDOMAIN answers)
 * NXDOMAIN answers may hold (among other data) chain of CNAME records. In order to find
 * to which domain exactly NXDOMAIN refers, we need to traverse this chain and find a member 
 * that doesn't appears as an 'owner' in one of CNAMEs
 *
 * Each entry in 'buf' has the following implicit structure:
 *
 *   struct CNAMEChainEntry {
 *       byte isOwner;
 *       byte size;
 *       char name[size + 1];
 *   };
 *  
 */
typedef struct {
    RvSize_t  chainSize;
    RvSize_t  bufSize;
    RvUint8  *buf;
    RvUint8  *nextFree;
    RvUint8   defaultBuf[512];
} RvCnameChain;


/* Internal data structure that is passed to type-specific decoders
 */
typedef struct {
    RvUint8 *msgBody;     /* msg start */
    RvSize_t    msgLen;   /* sizeof message */
    RvUint8 *msgCur;      /* current position in the message */
    RvCnameChain chain;  /* data structure that holds chain of CNAMEs */
} RvDecodeCtx;

typedef RvStatus (*RvDnsRecordDecoder)(RvDnsData *dr, RvDecodeCtx *ctx);

typedef struct {
    RvInt              dnsRecordType;               /* Record type aka DNS */
    RvDnsQueryType     internRecordType;            /* Data type of our internal representation */
    RvDnsRecordDecoder recordDecoder;
} RvDnsRecordDecoderEntry;

static 
RvStatus rvDnsDecodeARecord(RvDnsData *dr, RvDecodeCtx *ctx);

#if (RV_NET_TYPE & RV_NET_IPV6)
static RvStatus rvDnsDecodeAAAARecord(RvDnsData *dr, RvDecodeCtx *ctx);
#endif

static RvStatus rvDnsDecodeSRVRecord(RvDnsData *dr, RvDecodeCtx *ctx);
static RvStatus rvDnsDecodeNAPTRRecord(RvDnsData *dr, RvDecodeCtx *ctx);
static RvStatus rvDnsDecodeCNAMERecord(RvDnsData *dnsData, RvDecodeCtx *ctx);


/*static RvUint32 gsStatusTLS = 0;*/

/* Some of ARES clients (namely, SIP) aren't prepared to handle synchronous callbacks 
 * (e.g. callbacks from within ARES API calls) Those callbacks are caused by detecting erroneous
 * conditions in the time of sending DNS queries (for example, network unavailable). 
 * In order to prevent this situation without major reconstruction of ARES code, exception mechanism
 * was developed. We're "remembering" that API call is in progress by setting appropriate TLS variable, when 
 * entering API call and cleaning it on leaving API call.  When erroneous condition is encountered, 
 * we check whether API call is in progress and if it is, we just return error status code without 
 * calling any callbacks.
 */ 
static
void rvAresInitException(RvStatus *s);

static
void rvAresCancelException(void);

static
RvBool rvAresSetException(RvStatus s);


/*
#define T_A				1
#define T_AAAA	        28
#define T_SRV			33
#define T_NAPTR			35
*/
static const RvDnsRecordDecoderEntry gsRecordDecodersRegistry[] = 
{
    {T_A, RV_DNS_HOST_IPV4_TYPE, rvDnsDecodeARecord},

#if (RV_NET_TYPE & RV_NET_IPV6)
    {T_AAAA, RV_DNS_HOST_IPV6_TYPE, rvDnsDecodeAAAARecord},
#endif

    {T_SRV, RV_DNS_SRV_TYPE, rvDnsDecodeSRVRecord},
    {T_NAPTR, RV_DNS_NAPTR_TYPE, rvDnsDecodeNAPTRRecord},
    {T_CNAME, RV_DNS_CNAME_TYPE, rvDnsDecodeCNAMERecord},
    {-1, RV_DNS_UNDEFINED, 0}  /* should be last */
};



#define MAX_DOMAIN_SIZE     MAXCDNAME+1

typedef struct {
    /* The first part of this struct defines the arguments passed to rvDnsSearch, */
    /* dnsName will be stored immediately after the end of this struct */
    RvInt           queryType;
    void*           context;
    RvChar*         queryBuffer;
    RvSize_t        qbufLen;
    RvInt           status_as_is;		/* error status from trying as-is */
    RvInt           next_domain;		/* next search domain to try */
    RvInt           trying_as_is;		/* current query is for name as-is */
    RvUint32        domainMask;
    RvBool          tryAsIs;            /* tryAsIs == RV_TRUE - as is query should be tried */
} rvSearchQuery;



static
rvQuery* rvAresResetServers1(RvDnsEngine *dnsEngine);

static 
void rvAresResetServers2(RvDnsEngine *dnsEngine, rvQuery *canceledQueries);


/********************************************************************************************
 *
 *                                  Private functions
 *
 ********************************************************************************************/


static 
void RvDnsSearchSetMask(rvSearchQuery *q, RvUint32 mask) {
    q->tryAsIs = mask & 1;
    q->domainMask = mask >> 1;
    if(!q->tryAsIs) {
        q->status_as_is = ARES_ENOTFOUND;
    }
}

static
RvChar *RvDnsSearchGetNextDomain(RvDnsEngine *engine, RvSize_t nameLen, rvSearchQuery *q) {
    RvUint32 bit = 1 << q->next_domain;
    RvInt i;

    for(i = q->next_domain; i < engine->ndomains; i++, bit <<= 1) {
        RvSize_t suffixLen;

        if(!(q->domainMask & bit)) {
            continue;
        }

        suffixLen = strlen(engine->domains[i]);
        if(nameLen + suffixLen + 1 >= MAX_DOMAIN_SIZE) {
            continue;
        }
        
        break;

    }

    q->next_domain = i + 1;

    if(i >= engine->ndomains) {
        return 0;
    }

    return engine->domains[i];
}

/*****************************  
 * RvCnameChain class methods
 *****************************/

static 
void RvCnameChainConstruct(RvCnameChain *self) {
    self->chainSize = 0;
    self->bufSize   = sizeof(self->defaultBuf);
    self->buf       = self->defaultBuf;
    self->nextFree  = self->buf;
}


static 
void RvCnameChainDestruct(RvCnameChain *self) {
    RvLogMgr *logMgr = 0;
    if(self->buf != self->defaultBuf) {
        RvMemoryFree(self->buf, logMgr);
    }
}

/* Finds 'name' in CNAME chain given by 'self'
 * Returns pointer to 
 */
static 
RvUint8* RvCnameFind(RvCnameChain *self, RvChar *name, RvSize_t nameSize) {
    RvSize_t chainSize = self->chainSize;
    RvSize_t i;
    RvUint8 *cur;

    cur = self->buf;
    for(i = 0; i < chainSize; i++) {
        /* 'cur' points to 'isOwner' field, *(cur + 1) - to size field */
        RvSize_t curSize = *(cur + 1);
        RvChar  *curName = (RvChar *)(cur + 2);
        if(curSize == nameSize && !RvStrncasecmp(curName, name, curSize)) {
            return cur;
        }

        cur += curSize + 3;
    }

    return 0;
}

/* Find an entry that never appeared as 'owner' */
static
RvChar* RvCnameChainFindNxdomain(RvCnameChain *self, RvSize_t *pSize) {
    RvSize_t chainSize = self->chainSize;
    RvSize_t i;
    RvUint8 *cur;

    cur = self->buf;
    for(i = 0; i < chainSize; i++) {
        /* Never appeared as owner */
        if(*cur == RV_FALSE) {
            if(pSize) {
                *pSize = (RvSize_t)*(cur + 1);
            }
            return (RvChar *)(cur + 2);
        }

        cur += *(cur + 1) + 3;
    }

    return 0;
}


/* Adds single entry to CNAME chain */
static 
RvStatus RvCnameChainAddEntry(RvCnameChain *self, RvChar *name, RvSize_t nameSize, RvBool isOwner) {
    RvSize_t spaceLeft = self->bufSize - (self->nextFree - self->buf);
    RvSize_t spaceNeeded = nameSize + 3;
    RvLogMgr *logMgr = 0;
    RvStatus s = RV_OK;
    RvUint8  *entry;

    if(spaceLeft < spaceNeeded) {
        void *newBuf;
        RvSize_t newSize = self->bufSize << 1;

        
        s = RvMemoryAlloc(0, newSize, logMgr, &newBuf);
        if(s != RV_OK) {
            return s;
        }

        memcpy(newBuf, self->buf, self->bufSize);
        if(self->buf != self->defaultBuf) {
            RvMemoryFree(self->buf, logMgr);
        }
        self->bufSize = newSize;
        self->nextFree = (RvUint8 *)newBuf + (self->nextFree - self->buf);
        self->buf = (RvUint8 *)newBuf;
    }


    entry = self->nextFree;
    *entry++ = (RvUint8)isOwner;
    *entry++ = (RvUint8)nameSize;
    strcpy((RvChar *)entry, name);
    self->nextFree = entry + nameSize + 1;
    self->chainSize++;
    return s;
}

static
RvStatus RvCnameChainAdd(RvCnameChain *self, RvChar *owner, RvChar *alias) {
    RvSize_t nameSize = strlen(owner);
    RvUint8  *entry = RvCnameFind(self, owner, nameSize);
    RvStatus s = RV_OK;

    if(entry == 0) {
        s = RvCnameChainAddEntry(self, owner, nameSize, RV_TRUE);
        if(s != RV_OK) {
            return s;
        }
    } else {
        *entry = RV_TRUE;
    }

    nameSize = strlen(alias);
    entry = RvCnameFind(self, alias, nameSize);
    if(entry != 0) {
        return s;
    }

    s = RvCnameChainAddEntry(self, alias, nameSize, RV_FALSE);
    return s;
}


static
void RvDecodeCtxConstruct(RvDecodeCtx *self, RvUint8 *msg, RvSize_t msgLen) {
    self->msgCur = self->msgBody = msg;
    self->msgLen = msgLen;
    RvCnameChainConstruct(&self->chain);
}

static
void RvDecodeCtxDestruct(RvDecodeCtx *self) {
    RvCnameChainDestruct(&self->chain);
}


/********************************************************************************************
 * rvDnsDecodeRecord
 *
 * Fills record-type independent part of RvDnsData structure and
 * calls type-specific function to fill the rest
 *
 * INPUT:
 *  ctx  - decoding context (message, current position in the message, etc)
 * OUTPUT:
 *  data - points to RvDnsData structure that will contain decoded record
 * RETURNS:
 *  RV_OK on success, other values on failure
 *
 * Possible error codes:
 *
 * RV_DNS_ERROR_RTNOTSUPP - No support for this record type 
 *                         (currently supported types are CNAME, A, AAAA, NAPTR, SRV)
 * RV_ERROR_UNKNOWN       - generic error   
 */


static RvStatus rvDnsDecodeRecord(RvDnsData *data, RvDecodeCtx *ctx) {
    RvUint8* body;
    RvInt nameLen, type, dataLen;
    const RvDnsRecordDecoderEntry *pDecoderEntry;
    RvStatus s = RV_OK;

    body = ctx->msgCur;

    /* retrieve the record name */
    nameLen = ares_expand_name(body, ctx->msgBody, ctx->msgLen, data->ownerName, sizeof(data->ownerName));
    if (nameLen < 0) {
        return RV_DNS_ERROR_RMALFORMED;
    }
    body += nameLen;

    /* retrieve type & record length */
    type      = DNS_RR_TYPE(body);  /* type */
    dataLen   = DNS_RR_LEN(body);   /* data length */

    data->ttl = DNS_RR_TTL(body);  
    data->dataType = (RvDnsQueryType)type;
    ctx->msgCur = body + RRFIXEDSZ;

    pDecoderEntry = &gsRecordDecodersRegistry[0];
    for(pDecoderEntry = &gsRecordDecodersRegistry[0]; 
        pDecoderEntry->recordDecoder && pDecoderEntry->dnsRecordType != type;
        pDecoderEntry++) {
        }

    /* no decoder found - return 'record type not supported' error */
    if(pDecoderEntry->recordDecoder != 0) {
        s = pDecoderEntry->recordDecoder(data, ctx);
    } else {
        s = RV_DNS_ERROR_RTNOTSUPP; 
    }

    /* skip to the next record */
    ctx->msgCur += dataLen;
    return s;
}

/*********************************************************************************************
 * rvDnsDecodeARecord
 *
 * Type-specific decoder for A message type. Fills type-specific part of RvDnsData
 *
 * INPUT:
 *  ctx  - decoding context (message, current position in the message, etc)
 * OUTPUT:
 *  data - points to RvDnsData structure that will contain decoded record
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
 
static RvStatus rvDnsDecodeARecord(RvDnsData *dnsData, RvDecodeCtx *ctx) {
    RvUint32 ipAddr;

    memcpy(&ipAddr, ctx->msgCur, sizeof(ipAddr));
    RvAddressConstructIpv4(&dnsData->data.hostAddress, ipAddr, 0);
    return RV_OK;
}

#if (RV_NET_TYPE & RV_NET_IPV6)

/*********************************************************************************************
 * rvDnsDecodeAAAARecord
 *
 * Type-specific decoder for AAAA message type. Fills type-specific part of RvDnsData
 *
 * INPUT:
 *  ctx  - decoding context (message, current position in the message, etc)
 * OUTPUT:
 *  data - points to RvDnsData structure that will contain decoded record
 * RETURNS:
 *  RV_OK on success, other values on failure
 */

static 
RvStatus rvDnsDecodeAAAARecord(RvDnsData *dnsData, RvDecodeCtx *ctx) {
    RvAddressConstructIpv6(&dnsData->data.hostAddress, ctx->msgCur, 0, 0);
    return RV_OK;
}

#endif /* (RV_NET_TYPE & RV_NET_IPV6) */

/*********************************************************************************************
 * rvDnsDecodeSRVRecord
 *
 * Type-specific decoder for SRV message type. Fills type-specific part of RvDnsData
 *
 * INPUT:
 *  ctx  - decoding context (message, current position in the message, etc)
 * OUTPUT:
 *  data - points to RvDnsData structure that will contain decoded record
 * RETURNS:
 *  RV_OK on success
 *  RV_DNS_ERROR_RMALFORMED - malformed record
 */

static 
RvStatus rvDnsDecodeSRVRecord(RvDnsData *dnsData, RvDecodeCtx *ctx) {
    RvUint8* body;
    RvDnsSrvData* dnsSrvData;
    RvInt nameLen;

    dnsSrvData = &dnsData->data.dnsSrvData;
    body = ctx->msgCur;

    /* retrieve priority, weight & port number */
    dnsSrvData->priority = DNS__16BIT(body);   /* priority */
    dnsSrvData->weight   = DNS__16BIT(body+2); /* weight */
    dnsSrvData->port     = DNS__16BIT(body+4); /* port */

    /* retrieve real destination host name */
    nameLen = ares_expand_name(body + 6, ctx->msgBody, ctx->msgLen,
                               dnsSrvData->targetName, sizeof(dnsSrvData->targetName));
    if (nameLen < 0) {
        return RV_DNS_ERROR_RMALFORMED;
    }

    return RV_OK;
}

/*********************************************************************************************
 * rvDnsDecodeNAPTRRecord
 *
 * Type-specific decoder for NAPTR message type. Fills type-specific part of RvDnsData
 *
 * INPUT:
 *  ctx  - decoding context (message, current position in the message, etc)
 * OUTPUT:
 *  data - points to RvDnsData structure that will contain decoded record
 * RETURNS:
 *  RV_OK on success, other values on failure
 */

static 
RvStatus rvDnsDecodeNAPTRRecord(RvDnsData *dnsData, RvDecodeCtx *ctx) {
    RvUint8 *body = ctx->msgCur;
    RvDnsNaptrData *dnsNaptrData;
    RvInt nameLen;

    dnsNaptrData = &dnsData->data.dnsNaptrData;
    
    /* retrieve order & preference */
    dnsNaptrData->order      = DNS__16BIT(body);   /* order */
    dnsNaptrData->preference = DNS__16BIT(body+2); /* preference */
    body += 4;  /* update body pointer */

    /* retrieve Flags */
    nameLen = ares_expand_string(body, dnsNaptrData->flags, sizeof(dnsNaptrData->flags));
    if (nameLen < 0)
        return RV_DNS_ERROR_RMALFORMED;
    body += nameLen;  /* update body pointer */

    /* retrieve Service */
    nameLen = ares_expand_string(body, dnsNaptrData->service, sizeof(dnsNaptrData->service));
    if (nameLen < 0)
        return RV_DNS_ERROR_RMALFORMED;
    body += nameLen;  /* update body pointer */
    
    /* retrieve Regexp */
    nameLen = ares_expand_string(body, dnsNaptrData->regexp, sizeof(dnsNaptrData->regexp));
    if (nameLen < 0)
        return RV_DNS_ERROR_RMALFORMED;
    body += nameLen;  /* update body pointer */

    /* retrieve Replacement */
    nameLen = ares_expand_name(body, ctx->msgBody, ctx->msgLen,
                               dnsNaptrData->replacement, sizeof(dnsNaptrData->replacement));
    if (nameLen < 0)
        return RV_DNS_ERROR_RMALFORMED;

    return RV_OK;
}

static
RvStatus rvDnsDecodeCNAMERecord(RvDnsData *dnsData, RvDecodeCtx *ctx) {
  RvUint8 *body = ctx->msgCur;
  RvDnsCNAMEData *dnsCnameData = &dnsData->data.dnsCnameData;
  RvInt nameLen;
  RvStatus s = RV_OK;

  nameLen = ares_expand_name(body, ctx->msgBody, ctx->msgLen, dnsCnameData->alias, sizeof(dnsCnameData->alias));
  if(nameLen < 0) {
    return RV_DNS_ERROR_RMALFORMED;
  }

  /* Add new entry into CNAME chain if needed */
  s = RvCnameChainAdd(&ctx->chain, dnsData->ownerName, dnsCnameData->alias);

  return s;
}


/********************************************************************************************
 * rvDnsDecode
 *
 * Decodes the next record in DNS response 
 * 
 * INPUT:
 *  queryType - type of query (enum value)
 *  ctx       - decode context
 *  dnsData   - will hold decoded data
 * OUTPUT:
 *  msgBody      - a pointer to the current record in the result buffer
 *  dnsData      - DNS data structure (union of IPv4/6, SRV and NAPTR)
 *
 * RETURNS:
 *  RV_OK on success, other values on failure
 *
 * Possible error codes:
 *
 * RV_DNS_ERROR_RTNOTSUPP - No support for this record type 
 *                         (currently supported types are CNAME, A, AAAA, NAPTR, SRV)
 * 
 * RV_DNS_ERROR_RTUNEXPECTED - Resource type in response is different from resource type in question  
 * RV_DNS_ERROR_RMALFORMED   - Malformed record, unable to decode
 * RV_ERROR_UNKNOWN          - generic error   
 *
 */


static RvStatus rvDnsDecode(
    IN    RvInt           queryType,
          RvDecodeCtx    *ctx,
    OUT   RvDnsData      *dnsData)
{
    RvStatus status = RV_OK;

    dnsData->queryType = queryType;
    status = rvDnsDecodeRecord(dnsData, ctx);

    if(status != RV_OK) {
        dnsData->dataType = RV_DNS_STATUS_TYPE;
        dnsData->data.status = status;
        return status;
    }

    if(dnsData->dataType != queryType && dnsData->dataType != RV_DNS_CNAME_TYPE) {
        dnsData->dataType = RV_DNS_STATUS_TYPE;
        dnsData->data.status = RV_DNS_ERROR_RTUNEXPECTED;
        return RV_DNS_ERROR_RTUNEXPECTED;
    }
    
    return RV_OK;
}


/* RvUint32 rvDnsFindNxdomainTTL(RvDecodeCtx *ctx) 
 *
 * Finds TTL in the case of NXDOMAIN/NO DATA responses
 * In both cases TTL that will be returned refers to the minimum between 
 *  SOA's record own TTL and 'MINIMUM' field in SOA RDATA:
    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    /                     MNAME                     /
    /                                               /
    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    /                     RNAME                     /
    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    |                    SERIAL                     |
    |                                               |
    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    |                    REFRESH                    |
    |                                               |
    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    |                     RETRY                     |
    |                                               |
    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    |                    EXPIRE                     |
    |                                               |
    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    |                    MINIMUM                    |
    |                                               |
    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *
 *
 * ARGUMENTS
 *
 * ctx - decode context. We assume that decode context points now to the authority
 *       section in DNS response
 * 
 * RETURN
 * TTL of the first SOA record in authority section
 *
 */

static
RvUint32 rvDnsFindNxdomainTTL(RvDecodeCtx *ctx) {
    RvUint8 *msgBody = ctx->msgBody;
    RvUint8 *msgCur  = ctx->msgCur;
    RvSize_t msgLen  = ctx->msgLen;
    RvUint8 *msgEnd = msgBody + msgLen;
    RvSize_t nAuth = DNS_HEADER_NSCOUNT(msgBody);
    RvSize_t i;
    RvInt    nameLen;
    RvUint16 recType;
    RvUint16 recClass;
    RvUint32 ttl = 0;
    RvUint16 rdlen;

    for(i = 0; i < nAuth; i++) {
        nameLen = ares_enc_length(msgCur, msgBody, msgLen);
        if(nameLen < 0) {
            return 0;
        }
        /* Skip authority name */
        msgCur += nameLen;
        /* Currently we're at 'TYPE' field. There should be at least 
         * 2 byte TYPE field, 2 byte CLASS field, 4 byte TTL field 
         * and 2 byte RDLENGTH field
         */
        if(msgCur + 10 > msgEnd) {
            return 0;
        }

        recType = DNS_RR_TYPE(msgCur);
        recClass = DNS_RR_CLASS(msgCur);

        if(recType == T_SOA && recClass == C_IN) {
            RvUint32 soaTTL;
            RvUint32 minTTL;

            /* We got SOA record 
             * Set soaTTL to the TTL of the record itself
             */
            soaTTL = DNS_RR_TTL(msgCur);
            rdlen = DNS_RR_LEN(msgCur);
            /* Set msgCur to point 1 byte after SOA record */
            msgCur += 10 + rdlen;
            if(msgCur > msgEnd) {
                /* Illegal record */
                return 0;
            }
            /* We're interested in 'MINIMUM' field - last 4-byte field in SOA record */
            msgCur -= 4;
            minTTL = DNS__32BIT(msgCur);
            ttl = RvMin(soaTTL, minTTL);
            break;
        }

        /* Current record isn't SOA, proceed to the next */
        rdlen = DNS_RR_LEN(msgCur);
        msgCur += rdlen + 10;
    }

    ctx->msgCur = msgCur;
    return ttl;
}

/************************************************************************
 * rvDnsSkipQuestions(RvDecodeCtx *decodeCtx, RvInt *pQueryType, RvChar *name, RvSize_t nameLen)
 *
 * Skips question part of DNS message
 *
 * decodeCtx  - decoding context
 * pQueryType - (output) on return accepts a type of the last query
 * name       - (output) on return accepts an owner name of the last query
 * nameLen    - (input)  size of the 'name' buffer
 *
 * Return value
 *
 * RV_TRUE  - on success,
 * RV_FALSE - on failure (probably, malformed message)
 * 
 */
RvBool rvDnsSkipQuestions(RvDecodeCtx *decodeCtx, RvInt *pQueryType, RvChar *name, RvSize_t nameLen) {
    RvUint8 *msgCur = decodeCtx->msgCur;
    RvUint8 *msgBody = decodeCtx->msgBody;
    RvSize_t    msgSize = decodeCtx->msgLen;
    RvUint16 qdcount;
    RvInt    encLen;

    qdcount = DNS_HEADER_QDCOUNT(msgBody);

    while(qdcount > 0) {
        encLen = ares_expand_name(msgCur, msgBody, msgSize, name, (int)nameLen);
        if(encLen < 0) {
            return RV_FALSE;
        }
        msgCur += encLen;

        *pQueryType = DNS_QUESTION_TYPE(msgCur);  /* assuming qdcount == 1 (see ares_mkquery()) */

        msgCur += QFIXEDSZ;
        qdcount--;
    }

    decodeCtx->msgCur = msgCur;
    return RV_TRUE;
}

/*
 * void rvDnsTreatNxdomain(RvDnsEngine *dnsEngine, RvUint32 queryId, RvUint8 *abuf, RvSize_t alen) 
 * Treats NXDOMAIN responses, in particular caches them in DNS cache
 *
 * ARGUMENTS:
 *
 * dnsEngine - points to RvDnsEngine instance
 * queryId   - query ID of appropriate DNS query
 * abuf      - points to DNS response
 * alen      - response size (size of 'abuf')
 *
 * 
 */
void rvDnsTreatNxdomain(RvDnsEngine *dnsEngine, RvUint8 *abuf, RvSize_t alen) {
    RvDecodeCtx decodeCtx;
    RvInt qtype;
    RvInt ancount;
    RvDnsData dnsData;
    RvInt i;
    RvStatus s = RV_OK;
    RvAresCacheClt *cached = &dnsEngine->cache;
    RvAresCacheCtx  cacheCtx;
    RvInt rcode;


    RvDecodeCtxConstruct(&decodeCtx, abuf, alen);
    decodeCtx.msgCur += HFIXEDSZ;
    rcode = DNS_HEADER_RCODE(abuf);

    /* Skip question part, remembering owners name in dnsData.ownerName */
    if(!rvDnsSkipQuestions(&decodeCtx, &qtype, dnsData.ownerName, sizeof(dnsData.ownerName))) {
        return;
    }

    ancount = DNS_HEADER_ANCOUNT(abuf);
    dnsData.recordNumber = 0;
    (void)RvAresCacheCltStartCaching(cached, &cacheCtx);

    /* There may be CNAME records in the answer part,  
     * so traverse the answer section, remembering CNAME chain in decodeCtx.chain
     */
    for(i = 0; i < ancount; i++) {
        s = rvDnsDecode(qtype, &decodeCtx, &dnsData);
        if(s != RV_OK) {
            break;
        }

        s = RvAresCacheCltRecord(cached, &cacheCtx, &dnsData);
        if(s != RV_OK) {
            break;
        }
    }

    if(s == RV_OK) {
        RvChar *name;
       
        /* Find to which name refers NXDOMAIN response. This may be different
         * from the owner name in case CNAME chain exists
         */
        name = RvCnameChainFindNxdomain(&decodeCtx.chain, 0);
        /* Find TTL */
        dnsData.ttl  = rvDnsFindNxdomainTTL(&decodeCtx);
        dnsData.dataType = RV_DNS_STATUS_TYPE;
        dnsData.queryType = qtype;
        dnsData.data.status = rcode == NXDOMAIN ? ARES_ENOTFOUND : ARES_ENODATA;
        if(name != 0) {
            strcpy(dnsData.ownerName, name);
        }
        
        (void)RvAresCacheCltRecord(cached, &cacheCtx, &dnsData);
    }

    RvAresCacheCltFinishCaching(cached, &cacheCtx);
    RvDecodeCtxDestruct(&decodeCtx);
}

/********************************************************************************************
 * rvDnsCallback
 *
 * Callback routine called by the DNS resolver to handle query replies.
 *
 * INPUT:
 *  dnsEngine    - the DNS engine which is the "owner" of the source query
 *  context      - the context parameter provided by the user to RvAresSendQuery
 *  queryStatus       - the result status
 *  queryResults - the result buffer
 *  alen         - the result buffer length
 */
static void rvDnsCallback(
    IN  RvDnsEngine*    dnsEngine,
    IN  RvDnsNewRecordCB newRecordCB,
    IN  void*           context,
    IN  RvStatus        queryStatus,
    IN  RvUint32        queryId,
    IN  RvUint8*        queryResults,
    IN  RvInt           alen)
{
    RvInt i, ancount, queryType = 0;
    RvChar queryName[RV_DNS_MAX_NAME_LEN+1];
    RvDnsData dnsData;
    RvBool continueCaching = RV_TRUE;
    RvBool continueCallbacks = RV_TRUE; 
    RvStatus status =  RV_OK;
    RvDecodeCtx decodeCtx;
    RvAresCacheCtx cacheCtx;
    RvAresCacheClt *cached = &dnsEngine->cache;

    if(newRecordCB == 0) {
        newRecordCB = dnsEngine->newRecordCB;
    }

    memset(&dnsData, 0, sizeof(dnsData));
    
    if (queryStatus != ARES_SUCCESS) {
        /* We was called in-line, don't process callbacks,
         *  just set appropriate status and return.
         */
        RvBool isSync = rvAresSetException(queryStatus);
        if(isSync) {
            return;
        }
        dnsData.dataType    = RV_DNS_STATUS_TYPE;
        dnsData.data.status = queryStatus;
        dnsData.queryType = 0;
        dnsData.ttl = 0xffffffff;
        newRecordCB(context, queryId, &dnsData);
        return;
    }

    RvDecodeCtxConstruct(&decodeCtx, queryResults, alen);
    decodeCtx.msgCur += HFIXEDSZ; 

    /* skip the question part */
    if(!rvDnsSkipQuestions(&decodeCtx, &queryType, queryName, sizeof(queryName))) {
        return;
    }

    ancount = DNS_HEADER_ANCOUNT(queryResults);

    dnsData.recordNumber = 0;

    (void)RvAresCacheCltStartCaching(cached, &cacheCtx);

    for (i = 0; i < ancount; ++i)
    {

        /* decode the next DNS record and copy data into dnsData structure */
        status = rvDnsDecode(queryType, &decodeCtx, &dnsData);

        if (status == RV_OK)
        {
            RvStatus s;
            
            RvLogDebug(dnsEngine->dnsSource, (dnsEngine->dnsSource,
                "rvDnsCallback: calling user CB (queryId=%d,recNum=%d,dataType=%d)",
                queryId, dnsData.recordNumber, dnsData.dataType));
            
            if(continueCaching) {
                s = RvAresCacheCltRecord(cached, &cacheCtx, &dnsData);
                if(s != RV_OK) {
                    continueCaching = RV_FALSE;
                }
            }
            
            if(dnsData.dataType == queryType && continueCallbacks) {
                /* relevant answer found */
                dnsData.recordNumber++;
                status = newRecordCB(context, queryId, &dnsData);
                RvLogDebug(dnsEngine->dnsSource, (dnsEngine->dnsSource,
                    "rvDnsCallback: user CB returned (queryId=%d,status=%d)", queryId, status));

                if (RvErrorGetCode(status) == RV_ERROR_DESTRUCTED) {
                    continueCallbacks = RV_FALSE;
                }

            }
            
        } else if(status == RV_DNS_ERROR_RTNOTSUPP){
            /* Ignore not supported record types */
            status = RV_OK;
            continue;
        } else {
            /* Error occur during decoding record, most probably due
             *  to buffer overflow. Report error and return.
             *  May be we should proceed with other records?
             */
            RvLogError(dnsEngine->dnsSource, (dnsEngine->dnsSource, 
                "rvDnsCallback: unable to decode record (queryId=%d, status=%d)", queryId, status));
            
            newRecordCB(context, queryId, &dnsData);
            break;
        }
    }


    if(status == RV_OK) {
        if(dnsData.recordNumber == 0) {
            /* No relevant answers, apply negative caching */
            RvChar *name = RvCnameChainFindNxdomain(&decodeCtx.chain, 0);
            dnsData.ttl  = rvDnsFindNxdomainTTL(&decodeCtx);
            dnsData.dataType = RV_DNS_STATUS_TYPE;
            dnsData.data.status = ARES_ENODATA;
            dnsData.queryType = queryType;
            strcpy(dnsData.ownerName, name ? name : queryName);
            (void)RvAresCacheCltRecord(cached, &cacheCtx, &dnsData);
        } else {
            dnsData.dataType = RV_DNS_ENDOFLIST_TYPE;
        }

        newRecordCB(context, queryId, &dnsData);
    }

    (void)RvAresCacheCltFinishCaching(cached, &cacheCtx);
    RvDecodeCtxDestruct(&decodeCtx);
}



/********************************************************************************************
 * rvDnsCatDomain
 *
 * Concatenate two domains.
 *
 * INPUT:
 *  dnsName      - host name
 *  dnsDomain    - domain name
 *  buffSize     - the result buffer length
 * OUTPUT:
 *  buff         - the result buffer
 */
static RvStatus rvDnsCatDomain(
    IN  const RvChar*       dnsName,
    IN  const RvChar*       dnsDomain,
    IN  RvInt               buffSize,
    OUT RvChar*             buff)
{
    RvInt n;

    n = RvSnprintf(buff, buffSize, "%s.%s", dnsName, dnsDomain);
    return (n < 0 || n == buffSize) ? RV_ERROR_INSUFFICIENT_BUFFER : RV_OK;
}



/********************************************************************************************
 * rvDnsSearchCallback
 *
 * Callback routine called by the DNS resolver to handle domain suffixes.
 *
 * INPUT:
 *  dnsEngine    - the DNS engine which is the "owner" of the source query
 *  squery       - pointer to the search query state
 *  status       - the result status
 *  queryResults - the result buffer
 *  alen         - the result buffer length
 */
static void rvDnsSearchCallback(
    IN  RvDnsEngine*    dnsEngine,
    IN  RvDnsNewRecordCB newRecordCB,
    IN  void*           context,
    IN  RvStatus        status,
    IN  RvUint32        queryId,
    IN  RvUint8*        queryResults,
    IN  RvInt           alen)
{
    rvSearchQuery*  squery = (rvSearchQuery*)context;
    RvChar *dnsName;           /* points to the name embedded in the 'squery' */
    RvChar s[MAX_DOMAIN_SIZE]; /* temporary buffer for dnsName with appended suffix */
    RvSize_t nameLen;
    RvChar *domainSuffix;
    
    /* Keep searching unless we got a fatal error. */
    if(status != ARES_ENODATA && status != ARES_ESERVFAIL && status != ARES_ENOTFOUND && status != ARES_EREFUSED && status != ARES_ENDOFSERVERS)
    {
        rvDnsCallback(dnsEngine, newRecordCB, squery->context, status, queryId, queryResults, alen);
        return;
    }

    dnsName = (char*)squery + sizeof(*squery);
    nameLen = strlen(dnsName);

    /* Save the status if we were trying as-is. */
    if (squery->trying_as_is != 0)
        squery->status_as_is = status;

    /* We're accessing 'domains' information in dnsEngine structure, 
     * so, lock dnsEngine object
     */
    RvLockGet(&dnsEngine->lock, dnsEngine->logMgr);

    (void)rvDnsEngineSanityCheck(dnsEngine);
    
    domainSuffix = RvDnsSearchGetNextDomain(dnsEngine, nameLen, squery);

    if(domainSuffix == 0) {
        if(!squery->tryAsIs) {
            /* Nothing to try anymore */
            RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);
            RvLogDebug(dnsEngine->dnsSource,
                (dnsEngine->dnsSource, "rvDnsSearchCallback(engine=%p): No results found for %s", dnsEngine, dnsName));
            rvDnsCallback(dnsEngine, newRecordCB, squery->context, squery->status_as_is, queryId, queryResults, alen);
            return;
        }

        domainSuffix = "";
        squery->trying_as_is = RV_TRUE;
        squery->tryAsIs = RV_FALSE;
    } else {
        (void)rvDnsCatDomain(dnsName, domainSuffix, sizeof(s), s);
        dnsName = s;
    }

    RvLogDebug(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "rvDnsSearchCallback(engine=%p): Searching for %s, (domain suffix [%s])", dnsEngine, dnsName, domainSuffix));
        
    status = ares_query(dnsEngine, dnsName, C_IN, squery->queryType, squery->queryBuffer,
        &squery->qbufLen, newRecordCB, rvDnsSearchCallback, squery, queryId);

    (void)rvDnsEngineSanityCheck(dnsEngine);

    RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);
    if(status != RV_OK) {
        /* Some other error was returned by ares_query, report it and return*/
        rvDnsCallback(dnsEngine, newRecordCB, squery->context, status, queryId, 0, 0);
    }
}

RvSize_t RvAresComputeQuerySize(RvDnsEngine *channel, const char *name, const char *suffix);


/********************************************************************************************
 * rvDnsSearch
 *
 * Checks if a domain name suffix is needed and then sends a DNS query.
 *
 * INPUT:
 *  dnsEngine   - a DNS engine structure constructed previously
 *  dnsQuery    - type of query (enum value: IPv4/6, SRV or NAPTR)
 *  dnsName     - the name of the domain to search for its DNS records
 *  domainsMask - search should be performed on ith domain only if bit (i + 1) of this 
 *                mask is set. Bit 0 is reserved for search asis
 *  context     - a user private data. will be delivered to the user callbacks
 * OUTPUT:
 *  queryId     - the query id; generated by ARES
 * RETURNS:
 *  RV_OK on success, other values on failure
 */

static RvStatus rvDnsSearch(
                            IN  RvDnsEngine*        dnsEngine,
                            IN  RvInt               queryType,
                            IN  const RvChar*       dnsName,
                            IN  RvUint32            domainsMask,
                            IN  RvChar*             queryBuffer,
                            IN  RvSize_t*           qbufLen,
                            IN  RvDnsNewRecordCB    newRecordCB,
                            IN  void*               context,
                            OUT RvUint32            queryId)
{
    RvStatus status = RV_OK;
    RvInt ndots = 0;
    rvSearchQuery* squery;
    RvChar s[MAX_DOMAIN_SIZE];
    const RvChar *p;
    RvSize_t requiredMemory;
    RvSize_t maxQuerySize,len;
    RvSize_t nameSize;
    RvBool tryingAsIs = RV_FALSE;
    const RvChar *searchName = dnsName;
    RvChar *domainSuffix = 0;
    

    
    /* If name only yields one domain to search, then we don't have
     * to keep extra state, so just call ares_query().
     */
    nameSize = len = strlen(dnsName);
    if (dnsName[len - 1] == '.' || dnsEngine->ndomains == 0 || domainsMask == 1 || dnsEngine->flags & ARES_FLAG_NOSEARCH)
    {
        RvLogDebug(dnsEngine->dnsSource,
            (dnsEngine->dnsSource, "rvDnsSearch(engine=%p): Searching for %s as is (no suffix)", dnsEngine, dnsName));
        return ares_query(dnsEngine, dnsName, C_IN, queryType, queryBuffer,
                          qbufLen, newRecordCB, rvDnsCallback, context, queryId);
    }
    
    /* Try to allocate a space for rvSearchQuery structure (using queryBuffer) to hold the
     * state necessary for doing multiple lookups and the name string provided by the caller.
     */
    squery = (rvSearchQuery*)RvAlign(queryBuffer);
    len += sizeof(*squery) + 1 + ((RvChar *)squery - queryBuffer);  /* 1 byte for the null termination char. */

    /* make sure the address given to ares_query() will be aligned to 8 bytes */
    len = RvAlignValue(len);

    maxQuerySize = RvAresComputeQuerySize(dnsEngine, dnsName, dnsEngine->domains[dnsEngine->longest_domain_i]);
    requiredMemory = len + maxQuerySize;

    if(requiredMemory > (RvSize_t)*qbufLen) {
        *qbufLen = requiredMemory;
        return ARES_ENOMEM;
    }
    
    squery->qbufLen = *qbufLen - len;
    squery->queryType    = queryType;
    squery->context      = context;
    squery->queryBuffer  = (RvChar *)squery + len;
    squery->status_as_is = -1;
    squery->next_domain = 0;
    RvDnsSearchSetMask(squery, domainsMask);

    
    strcpy((char*)squery + sizeof(*squery), dnsName);
    
    /* Count the number of dots in name. */
    for (p = dnsName; *p; p++) if (*p == '.') ndots++;
    
    /* If ndots is at least the channel ndots threshold (usually 1),
     * then we try the name as-is first.  Otherwise, we try the name
     * as-is last.
     */
    tryingAsIs = squery->trying_as_is = ndots >= dnsEngine->ndots;

    if(tryingAsIs) {
        domainSuffix = "";
    } else {
        domainSuffix = RvDnsSearchGetNextDomain(dnsEngine, nameSize, squery);
        if(domainSuffix == 0) {
            domainSuffix = "";
            squery->trying_as_is = tryingAsIs = RV_TRUE;
        }
    }

    if(tryingAsIs && !squery->tryAsIs) {
        /* shouldn't happen */
        return RV_OK;
    }

    if(tryingAsIs) {
        squery->tryAsIs = RV_FALSE;
    }
        
    /* Append domain suffix to the dnsName */
    if(domainSuffix[0]) {
        (void)rvDnsCatDomain(dnsName, domainSuffix, sizeof(s), s);
        searchName = s;
    }

    RvLogDebug(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "rvDnsSearch(engine=%p): Searching for %s with suffix [%s]", dnsEngine, searchName, domainSuffix));
    status = ares_query(dnsEngine, searchName, C_IN, queryType, squery->queryBuffer,
        &squery->qbufLen, newRecordCB, rvDnsSearchCallback, squery, queryId);
        
    if (status != ARES_SUCCESS) {
        /* Shouldn't happen */
        if (status == ARES_ENOMEM)
            *qbufLen = len + squery->qbufLen;
    }
        
    return status;
}


/* Set exception code
 * Returns RV_TRUE if exception variable exist. This means that no callbacks should
 * be called,
 * Otherwise - returns RV_FALSE
 */
static
RvBool rvAresSetException(RvStatus s) {
  RvStatus *ps;
  RV_USE_CCORE_GLOBALS;

  RvThreadGetVar(gsStatusTLS, 0, (void *)&ps);
  if(ps == 0) {
    return RV_FALSE;
  }

  *ps = s;
  return RV_TRUE;
}

static
void rvAresInitException(RvStatus *s) {
  RV_USE_CCORE_GLOBALS;
  *s = RV_OK;
  RvThreadSetVar(gsStatusTLS, s, 0);
}

static
void rvAresCancelException() {
  RV_USE_CCORE_GLOBALS;
  RvThreadSetVar(gsStatusTLS, 0, 0);
}




#endif /* RV_DNS_NONE */


/********************************************************************************************
 *
 *                                  Public functions
 *
 ********************************************************************************************/


/********************************************************************************************
 * RvAresInit
 *
 * Initializes the DNS module.
 *
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RvStatus RvAresInit(void)
{
    RvStatus status = RV_OK;

#if (RV_DNS_TYPE == RV_DNS_ARES)
	RV_USE_CCORE_GLOBALS;

  RvThreadCreateVar(0, "RvAresStatus", 0, &gsStatusTLS);
  status = ares_init();
  if(status != RV_OK) {
      return status;
  }

  status = RvAresCacheDInit();
#endif
    
    return status;
}


/********************************************************************************************
 * RvAresSourceConstruct
 *
 * Constructs a Log Source object for the DNS module.
 *
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RvStatus RvAresSourceConstruct(RvLogMgr* logMgr)
{
    RvStatus status = RV_OK;

#if (RV_DNS_TYPE == RV_DNS_ARES)
    status = RvLogSourceConstruct(logMgr, &logMgr->dnsSource, "ARES", "Asynchronous DNS resolving");
#else
    RV_UNUSED_ARG(logMgr);
#endif

    return status;
}




/********************************************************************************************
 * RvAresEnd
 *
 * Shutdown the DNS module.
 *
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RvStatus RvAresEnd(void)
{
    RvStatus s = RV_OK;
#if (RV_DNS_TYPE == RV_DNS_ARES)
    ares_end();
    s = RvAresCacheDEnd();
#endif
    
    return s;
}




#if (RV_DNS_TYPE == RV_DNS_ARES)


/********************************************************************************************
 * RvAresConstruct
 *
 * Constructs a DNS engine, allocates memory for the DNS server state structures,
 * allocates memory for TCP input data and sets the user callback routine which will
 * be called upon answers arrival.
 *
 * INPUT:
 *  selectEngine - select engine constructed by the user previously and will be
 *                 used to receive transport events from DNS servers
 *  newItemCB    - a user callback routine to handle DNS replies
 *  maxServers   - the maximum number of DNS servers that will be configured
 *                 will be set to the actual number of servers found in the system repository
 *  maxDomains   - the maximum number of domain strings that will be configured
 *                 will be set to the actual number of domains found in the system repository
 *  tcpBuffLen   - length of the TCP buffer that will be used to receive DNS replies
 *  logMgr       - handle of the log manager for this instance
 * OUTPUT:
 *  dnsEngine    - a structure of type RvDnsEngine allocated by the user
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RVCOREAPI RvStatus RVCALLCONV RvAresConstructN(
                                               IN  RvSelectEngine*     selectEngine,
                                               IN  RvDnsNewRecordCB    newRecordCB,
                                               IN  RvInt               maxServers,
                                               IN  RvInt               maxDomains,
                                               IN  RvInt               tcpBuffLen,
                                               IN  RvLogMgr*           logMgr,
                                               OUT RvDnsEngine*        dnsEngine)
{
    RvStatus status;
    struct ares_options options;
    RvInt optmask = 0;
    RvAresCacheParams cacheParams;
    
    if (logMgr != NULL)
    {
        RvLogEnter(&logMgr->dnsSource,
            (&logMgr->dnsSource, "RvAresConstruct(engine=%p)", dnsEngine));
    }
    
#if (RV_CHECK_MASK & RV_CHECK_NULL)
    if (selectEngine == NULL || newRecordCB == NULL || dnsEngine == NULL)
    {
        if (logMgr != NULL)
        {
            RvLogError(&logMgr->dnsSource, (&logMgr->dnsSource, "RvAresConstruct: NULL"));
        }
        return RV_ERROR_NULLPTR;
    }
#endif
    
#define FORCE_TCP_QUERIES 0
#if (FORCE_TCP_QUERIES == 1)
    /* --------------------- debugging --------------------- */
    options.flags  = 0;
    options.flags |= ARES_FLAG_USEVC;  /* use TCP */
    optmask |= ARES_OPT_FLAGS;
    /* --------------------- debugging --------------------- */
#endif
    
    dnsEngine->userQueryId  = 1;
    dnsEngine->selectEngine = selectEngine;
    dnsEngine->newRecordCB  = newRecordCB;
    
    RvSelectGetTimeoutInfo(selectEngine, NULL, &dnsEngine->timerQueue);
    
    dnsEngine->logMgr = logMgr;
    if (logMgr != NULL)
        dnsEngine->dnsSource = &logMgr->dnsSource;
    else
        dnsEngine->dnsSource = NULL;
    
    
    RvAresCacheParamsInit(&cacheParams);
    
    status = RvAresCacheCltConstruct(&dnsEngine->cache, selectEngine, &cacheParams, logMgr);
    if(status != RV_OK) {
        RvLogError(dnsEngine->dnsSource,
            (dnsEngine->dnsSource, "RvAresConstruct(%p,%d), cache creation failed", dnsEngine, status));
        return status;
    }
    
    if(maxDomains > RV_DNS_MAX_DOMAINS) {
        RvLogWarning(dnsEngine->dnsSource, 
            (dnsEngine->dnsSource, "RvAresConstruct(%p), too many domains required (maxDomains = %d), using 31 instead", dnsEngine, maxDomains));
        maxDomains = RV_DNS_MAX_DOMAINS;
    }
        
    status = ares_construct(dnsEngine, maxServers, maxDomains, tcpBuffLen, &options, optmask);
    if (status != ARES_SUCCESS) {
        RvLogError(dnsEngine->dnsSource,
            (dnsEngine->dnsSource, "RvAresConstruct(%p; %d)", dnsEngine, status));
        return RV_ERROR_UNKNOWN;
    }
    
    RvLogLeave(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresConstruct(engine=%p)", dnsEngine));
    
    return RV_OK;
}

RVCOREAPI RvStatus RVCALLCONV RvAresConstructO(
                                               IN    RvSelectEngine*    selectEngine,
                                               IN    RvDnsNewRecordCB   newRecordCB,
                                               INOUT RvInt*             maxServers,
                                               INOUT RvInt*             maxDomains,
                                               IN    RvInt              tcpBuffLen,
                                               IN    RvBool             retrieveDnsServers,
                                               IN    RvBool             retrieveDnsSuffix,
                                               IN    RvLogMgr*          logMgr,
                                               OUT   RvDnsEngine*       dnsEngine) {

    RvStatus s = RV_OK;
    RvDnsConfigType configType = 0;

    s = RvAresConstructN(selectEngine, newRecordCB, *maxServers, *maxDomains, tcpBuffLen, logMgr, dnsEngine);
    if(s != RV_OK) {
        return s;
    }

    configType |= (retrieveDnsSuffix ? RV_DNS_SUFFIXES : 0);
    configType |= (retrieveDnsServers ? RV_DNS_SERVERS : 0);

    s = RvAresConfigure(dnsEngine, configType);
    if(s != RV_OK) {
        return s;
    }
    
    *maxServers = dnsEngine->nservers;
    *maxDomains = dnsEngine->ndomains;
    return s;
 }


/********************************************************************************************
 * RvAresDestruct
 *
 * Destructs a DNS engine.
 *
 * INPUT:
 *  dnsEngine  - a DNS engine structure
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RVCOREAPI RvStatus RVCALLCONV RvAresDestruct(
    IN RvDnsEngine*          dnsEngine)
{
#if (RV_CHECK_MASK & RV_CHECK_NULL)
    if (dnsEngine == NULL)
        return RV_ERROR_NULLPTR;
#endif

    RvLogEnter(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresDestruct(engine=%p)", dnsEngine));

    RvAresCacheCltDestruct(&dnsEngine->cache);
    ares_destruct(dnsEngine);

    RvLogLeave(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresDestruct(engine=%p)", dnsEngine));

    return RV_OK;
}

#if RV_LOGMASK != RV_LOGLEVEL_NONE

/* Dumps configuration info to log */
static 
void rvAresDumpConfigInfo(RvDnsEngine *dnsEngine, RvDnsConfigType configType) {
#define LSRC dnsEngine->dnsSource

    rvServerState *cur;
    RvInt nservers = dnsEngine->nservers;
    RvInt ndomains = dnsEngine->ndomains;
    RvChar **curd;

    if(configType & RV_DNS_SERVERS) {
        if(nservers == 0) {
            RvLogWarning(LSRC, (LSRC, "No servers configured"));
        } else {
            RvLogDebug(LSRC, (LSRC, "%d servers configured", nservers));
        }

        for(cur = dnsEngine->servers; nservers; nservers--, cur++) {
            RvChar saddr[64];

            RvAddressGetString(&cur->addr, sizeof(saddr), saddr);

            RvLogDebug(LSRC, (LSRC, "   %s", saddr));
        }
    }

    if(configType & RV_DNS_SUFFIXES) {
        RvLogDebug(LSRC, (LSRC, "%d domains confugured", ndomains));

        for(curd = dnsEngine->domains; ndomains--; curd++) {
            RvLogDebug(LSRC, (LSRC, "   %s", *curd));
        }
    }
}

#endif


/********************************************************************************************
 * RvAresConfigure
 *
 * Read the system configuration (DNS servers and suffix list) and set the values
 * in the DNS engine.
 * This function completes the construction of a DNS engine.
 *
 * INPUT:
 *  dnsEngine    - a DNS engine structure
 *  configType   - a bit-mask indicating what part of the system configuration should be set.
 *                 allowable values are RV_DNS_SERVERS and RV_DNS_SUFFIXES
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RVCOREAPI RvStatus RVCALLCONV RvAresConfigure(
    IN  RvDnsEngine*        dnsEngine,
    IN  RvDnsConfigType     configType)
{
    RvStatus status;
    rvQuery *canceledQueries = 0;

#if (RV_CHECK_MASK & RV_CHECK_NULL)
    if (dnsEngine == NULL)
        return RV_ERROR_NULLPTR;
#endif
    
    RvLogEnter(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresConfigure(engine=%p)", dnsEngine));

    RvLockGet(&dnsEngine->lock, dnsEngine->logMgr);

    if(configType & RV_DNS_SERVERS) {
        canceledQueries = rvAresResetServers1(dnsEngine);
    }
    
    status = ares_configure(dnsEngine, configType);

#if RV_LOGMASK != RV_LOGLEVEL_NONE
    rvAresDumpConfigInfo(dnsEngine, configType);
#endif
    
    RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);

    if(canceledQueries) {
        rvAresResetServers2(dnsEngine, canceledQueries);
    }

    
    RvLogLeave(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresConfigure(engine=%p)=%d", dnsEngine, status));

    return status;
}



/********************************************************************************************
 * RvAresSetParams
 *
 * Change the default parameters for a DNS engine.
 *
 * INPUT:
 *  dnsEngine    - a DNS engine structure
 *  timeoutInSec - timeout in seconds for un-answered query
 *  nTries       - number of tries before canceling a query
 *  dnsServList  - array of DNS server addresses
 *  nServers     - number of DNS server addresses
 *  dnsDomains   - array of DNS domain suffixes
 *  nDomains     - number of DNS domain suffixes
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RVCOREAPI RvStatus RVCALLCONV RvAresSetParams(
    IN  RvDnsEngine*        dnsEngine,
    IN  RvInt               timeoutInSec,
    IN  RvInt               nTries,
    IN  RvAddress*          dnsServList,
    IN  RvInt               nServers,
    IN  RvChar**            dnsDomains,
    IN  RvInt               nDomains)
{
    RvStatus status;
    struct ares_options options;
    RvInt optmask = 0;
    RvBool resetServers = RV_FALSE;
    rvQuery *canceledQueries = 0;

#if (RV_CHECK_MASK & RV_CHECK_NULL)
    if (dnsEngine == NULL)
        return RV_ERROR_NULLPTR;
#endif

    RvLogEnter(dnsEngine->dnsSource, (dnsEngine->dnsSource,
               "RvAresSetParams(engine=%p,to=%d,nTries=%d,nServers=%d,nDomains=%d)",
               dnsEngine, timeoutInSec, nTries, nServers, nDomains));
    
    if (timeoutInSec != -1)
    {
        options.timeout = RvInt64Mul(RvInt64FromRvInt(timeoutInSec), RV_TIME64_NSECPERSEC);
        optmask        |= ARES_OPT_TIMEOUT;
    }

    if (nTries != -1)
    {
        options.tries = nTries;
        optmask      |= ARES_OPT_TRIES;
    }

    if (dnsServList != NULL)
    {
        resetServers = RV_TRUE;
        options.servers  = dnsServList;
        options.nservers = nServers;
        optmask         |= ARES_OPT_SERVERS;
    }

    if (dnsDomains != NULL)
    {
        options.domains  = dnsDomains;
        options.ndomains = nDomains;
        optmask         |= ARES_OPT_DOMAINS;
    }


    RvLockGet(&dnsEngine->lock, dnsEngine->logMgr);

    /* Resetting resolver is needed:
     * existing sockets will be closed and unregistered from select engine
     * queries will be canceled and appropriate callbacks raised
     */
    if(resetServers) {
        canceledQueries = rvAresResetServers1(dnsEngine);
    }

    status = ares_set_options(dnsEngine, &options, optmask);

    /* At this point, our resolver should be in consistent state again, unlock it */

    RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);

    /* Now we may call callbacks for canceled queries */

    if(canceledQueries) {
        rvAresResetServers2(dnsEngine, canceledQueries);
    }

    if (status != ARES_SUCCESS) {
        RvLogError(dnsEngine->dnsSource,
            (dnsEngine->dnsSource, "RvAresSetParams(%p; %d)", dnsEngine, status));
        return RV_ERROR_UNKNOWN;
    }

    RvLogLeave(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresSetParams(engine=%p)", dnsEngine));

    return RV_OK;
}



/********************************************************************************************
 * RvAresGetParams
 *
 * Retrieve the configuration parameters for a DNS engine.
 *
 * INPUT:
 *  dnsEngine    - a DNS engine structure
 * OUTPUT:
 *  timeoutInSec - timeout in seconds for un-answered query
 *  nTries       - number of tries before canceling a query
 *  dnsServList  - array of DNS server addresses
 *  nServers     - number of DNS server addresses
 *  dnsDomains   - array of DNS domain suffixes
 *  nDomains     - number of DNS domain suffixes
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RVCOREAPI RvStatus RVCALLCONV RvAresGetParams(
    IN     RvDnsEngine*     dnsEngine,
    OUT    RvInt*           timeoutInSec,
    OUT    RvInt*           nTries,
    OUT    RvAddress*       dnsServList,
    INOUT  RvInt*           nServers,
    OUT    char**           dnsDomains,
    INOUT  RvInt*           nDomains)
{
    RvInt64 timeout;

#if (RV_CHECK_MASK & RV_CHECK_NULL)
    if (dnsEngine == NULL)
        return RV_ERROR_NULLPTR;
#endif

    RvLogEnter(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresGetParams(engine=%p)", dnsEngine));


    RvLockGet(&dnsEngine->lock, dnsEngine->logMgr);

    ares_get_options(dnsEngine, &timeout, nTries, dnsServList, nServers, dnsDomains, nDomains);

    RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);

    if (timeoutInSec != NULL)
        *timeoutInSec = RvInt64ToRvInt(RvInt64Div((timeout), RV_TIME64_NSECPERSEC));

    RvLogLeave(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresGetParams(engine=%p)", dnsEngine));

    return RV_OK;
}



/********************************************************************************************
 * RvAresSendQuery
 *
 * Sends a DNS query to one of the pre-configured DNS servers and returns immediately
 * before replies are received.
 *
 * INPUT:
 *  dnsEngine   - a DNS engine structure constructed previously
 *  dnsQuery    - type of query (enum value: IPv4/6, SRV or NAPTR)
 *  dnsName     - the name of the domain to search for its DNS records
 *  as_is       - indicates whether ARES will use the "suffix algorithm" or send a query
 *                with the provided name as-is
 *  queryBuffer - a buffer of qbufLen bytes long to be used by ARES as a work area
 *                to generate the query.
 *                the buffer must not be corrupted by the user until a reply is received
 *  qbufLen     - specifies the length in bytes of queryBuffer.
 *                if length is too small an error is returned and the required value is
 *                set into this parameter.
 *                Note: this notification may occurred iteratively more than once
 *  context     - a user private data. will be delivered to the user callback
 * OUTPUT:
 *  queryId     - the query id; generated by ARES; enables the user to cancel transactions
 *                and also delivered to the user callback together with the context param.
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RVCOREAPI RvStatus RVCALLCONV RvAresSendQueryEx(
  IN    RvDnsEngine*      dnsEngine,
  IN    RvDnsQueryType    dnsQuery,
  IN    RvChar*           dnsName,
  IN    RvBool            as_is,
  IN    void*             queryBuffer,
  INOUT RvInt*            qbufLen,
  IN    RvDnsNewRecordCB  newRecordCB,
  IN    void*             context,
  OUT   RvUint32*         queryId)
{
    RvStatus status;
    RvStatus exception;
    RvStatus cacheStatus;
    RvInt queryType;
	RvSize_t tqbufLen = *qbufLen;
    RvChar **domainSuffixes;
    RvSize_t nDomains;
    RvUint32 domainMask = 0;
    RvAresCacheClt *cached = &dnsEngine->cache;
    RvSize_t nameSize = strlen(dnsName);




#if (RV_CHECK_MASK & RV_CHECK_NULL)
    if (dnsEngine == NULL)
        return RV_ERROR_NULLPTR;
#endif

    RvLogEnter(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresSendQuery(engine=%p,name=%s,query=%d)", dnsEngine, dnsName, dnsQuery));

    
    switch (dnsQuery)
    {
    case RV_DNS_HOST_IPV4_TYPE:
        queryType = T_A;
        break;
#if (RV_NET_TYPE & RV_NET_IPV6)
    case RV_DNS_HOST_IPV6_TYPE:
        queryType = T_AAAA;
        break;
#endif
    case RV_DNS_SRV_TYPE:
        queryType = T_SRV;
        break;
    case RV_DNS_NAPTR_TYPE:
        queryType = T_NAPTR;
        break;
    default:
        return RV_ERROR_BADPARAM;
    }

    status = RvLockGet(&dnsEngine->lock, dnsEngine->logMgr);
   
    (void)rvDnsEngineSanityCheck(dnsEngine);
    if(newRecordCB == 0) {
        newRecordCB = dnsEngine->newRecordCB;
    }

    if (status != RV_OK)
        return status;

    *queryId = dnsEngine->userQueryId++;


    rvAresInitException(&exception);

    if(as_is) {
        domainSuffixes = 0;
        nDomains = 0;
    } else {
        domainSuffixes = dnsEngine->domains;
        nDomains = dnsEngine->ndomains;
    }
    
    domainMask = (1 << (nDomains + 1)) - 1;

    /* First, try to find this record in cache */
    cacheStatus = RvAresCacheCltFind(cached, queryType, dnsName, nameSize, domainSuffixes,
                                     nDomains, &domainMask, newRecordCB, context, 
                                     *queryId, queryBuffer, &tqbufLen);

    /* If RV_OK - record was found in positive cache and results will be reported using callback mechanism
     *    RV_ERROR_INSUFFICIENT_BUFFER - record was found in positive cache, but external buffer is too
     *      small to keep query-related information
     *    RV_DNS_ERROR_NOTFOUND - record was found in negative cache. In this case the record may still be found 
     *    in the hosts file, so if searching hosts file is enabled we should proceed.
     */
    if(cacheStatus == RV_ERROR_INSUFFICIENT_BUFFER) {
        RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);
        *qbufLen = (RvInt)tqbufLen;
        return cacheStatus;
    }

    if(cacheStatus == RV_OK || cacheStatus == RV_DNS_ERROR_NOTFOUND) {
        /* This request will be served from cache - just return */
        RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);
        RvLogDebug(dnsEngine->dnsSource, 
                    (dnsEngine->dnsSource, "RvAresSendQuery: serving %d request for %s from cache", queryType, dnsName));
        return cacheStatus;
    }

    /* send the query */
    if (as_is == RV_TRUE) {
        status = ares_query(dnsEngine, dnsName, C_IN, queryType, queryBuffer,
                            &tqbufLen, newRecordCB, rvDnsCallback, context, *queryId);
    } else {
        status = rvDnsSearch(dnsEngine, queryType, dnsName, domainMask, queryBuffer, &tqbufLen, newRecordCB,
                             context, *queryId);
    }

	*qbufLen = (RvInt)tqbufLen;

    (void)rvDnsEngineSanityCheck(dnsEngine);

    RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);

    rvAresCancelException();

    if(status != ARES_SUCCESS && status != RV_DNS_ERROR_NOTFOUND)
    {
        if (status == ARES_ENOMEM)
        {
            RvLogDebug(dnsEngine->dnsSource,
                (dnsEngine->dnsSource, "RvAresSendQuery(%p; %d) - INSUFFICIENT_BUFFER", dnsEngine, status));
            return RV_ERROR_INSUFFICIENT_BUFFER;
        }
        

        RvLogError(dnsEngine->dnsSource,
            (dnsEngine->dnsSource, "RvAresSendQuery(%p; %d)", dnsEngine, status));
        return RV_ERROR_UNKNOWN;
    }

    if(exception != RV_OK) {
        RvLogError(dnsEngine->dnsSource, (dnsEngine->dnsSource, "RvAresSendQuery(%p; %d)", dnsEngine, status));
        return exception;
    }



    RvLogLeave(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresSendQuery(engine=%p,qid=%d)", dnsEngine, *queryId));

    return status;
}



/********************************************************************************************
 * RvAresCancelQuery
 *
 * Asks ARES to ignore replies for a query which has been already sent.
 * The query buffer provided by the user is no longer valid to ARES.
 *
 * INPUT:
 *  dnsEngine   - a DNS engine structure constructed previously
 *  queryId     - the query id supplied by ARES when the query was sent
 * RETURNS:
 *  RV_OK on success, other values on failure
 */
RVCOREAPI RvStatus RVCALLCONV RvAresCancelQuery(
	IN  RvDnsEngine*        dnsEngine,
    IN  RvUint32            queryId)
{
    RvStatus status;

#if (RV_CHECK_MASK & RV_CHECK_NULL)
    if (dnsEngine == NULL)
        return RV_ERROR_NULLPTR;
#endif

    RvLogEnter(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresCancelQuery(engine=%p,queryId=%d)", dnsEngine, queryId));

    status = RvLockGet(&dnsEngine->lock, dnsEngine->logMgr);
    if (status != RV_OK)
        return status;

    (void)rvDnsEngineSanityCheck(dnsEngine);

    status = RvAresCacheCltCancelQuery(&dnsEngine->cache, queryId);

    if(status == RV_DNS_ERROR_CACHE_NOTFOUND) {
        /* It's not cached query, or already canceled, try reto find among regular queries */
        status = ares_cancel_query(dnsEngine, queryId);
    }

    (void)rvDnsEngineSanityCheck(dnsEngine);

    RvLockRelease(&dnsEngine->lock, dnsEngine->logMgr);

    if (status != RV_OK) {
        RvLogDebug(dnsEngine->dnsSource, (dnsEngine->dnsSource,
                   "RvAresCancelQuery: ares_cancel_query failed(status=%d)", status));
    }

    
    RvLogLeave(dnsEngine->dnsSource,
        (dnsEngine->dnsSource, "RvAresCancelQuery(engine=%p)", dnsEngine));

    return status;
}


/* rvAresResetServers1
 * Resetting servers is executed in 2 stages:
 * 1 - Unregistering all servers from select engine and canceling all queries
 * 2 - For each canceled query appropriate user-supplied callback will be called
 * Such 2 stage operation is needed because stage 1 should be performed when the dns 
 *  engine is locked and the second - when the dns engine is open
 * 
 * rvAresResetServers1 performs the first task and rvAresResetServers2 - the second
 *
 * Return value of rvAresResetServers1 is a list of canceled queries, that serves
 *  as input for the second stage
 */

static
rvQuery* rvAresResetServers1(RvDnsEngine *dnsEngine) {
    RvInt i;
    RvInt nservers = dnsEngine->nservers;
    rvQuery *canceledQueries;
    
    for (i = 0; i < nservers; i++) {
        ares__close_sockets(dnsEngine, i);
    }
    
    /* Cancel all queries 
    * Remember the list in 'canceledQueries' for calling later callbacks
    * for canceled queries
    */
    canceledQueries = ares_cancel_queries(dnsEngine);
    
    return canceledQueries;
}

static 
void rvAresResetServers2(RvDnsEngine *dnsEngine, rvQuery *canceledQueries) {
    rvQuery *nextQuery = 0;
    rvQuery *q;
    RvDnsNewRecordCB newRecordCB;

    
    /* Now we may call callbacks for canceled queries */
    for(q = canceledQueries; q; q = nextQuery) {
        RvDnsData dnsData;
        void *context;
        RvUint32 queryId;

        /* query structure may be destroyed during a callback,
         * so save the next query in 'nextQuery' variable
         */
        nextQuery = q->qnext;
        dnsData.dataType = RV_DNS_STATUS_TYPE;
        dnsData.data.status = RV_DNS_QUERY_CANCELED;
        dnsData.queryType = 0;
        queryId = q->user_qid;
        context = q->user_arg;

        if(q->user_callback == rvDnsSearchCallback) {
            /* It's our internal callback q->user_arg points to the
             * rvSearchQuery structure that, in turn, holds relevant context
             */
            rvSearchQuery *sq = (rvSearchQuery *)context;
            context = sq->context;
        } 

        if((newRecordCB = q->newRecordCB) == 0) {
            newRecordCB = dnsEngine->newRecordCB;
        }

        newRecordCB(context, queryId, &dnsData);
    }
}



RVCOREAPI 
RvStatus RVCALLCONV RvAresClearCache(RvDnsEngine *dnsEngine) {
    RvAresCacheCltClear(&dnsEngine->cache);
    return RV_OK;
}
    
#endif /* RV_DNS_ARES */
