#include "uploader.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>
#include <pthread.h>
#include "servertime.h"
#include <time.h>
#include <curl/curl.h>
#ifdef __ARM
#include "./demo/socket_logging.h"
#endif

size_t getDataCallback(void* buffer, size_t size, size_t n, void* rptr);

#define TS_DIVIDE_LEN 4096

enum WaitFirstFlag {
        WF_INIT,
        WF_LOCKED,
        WF_FIRST,
        WF_QUIT,
};

typedef struct _KodoUploader{
        TsUploader uploader;
#ifdef TK_STREAM_UPLOAD
        CircleQueue * pQueue_;
#else
        char *pTsData;
        int nTsDataCap;
        int nTsDataLen;
#endif
        pthread_t workerId_;
        int isThreadStarted_;
        
        UploadArg uploadArg;

        int64_t nFirstFrameTimestamp;
        int64_t nLastFrameTimestamp;
        UploadState state;
        
        int64_t getDataBytes;
        curl_off_t nLastUlnow;
        int64_t nUlnowRecTime;
        int nLowSpeedCnt;
        int nIsFinished;
        
        pthread_mutex_t waitFirstMutex_;
        enum WaitFirstFlag nWaitFirstMutexLocked_;
}KodoUploader;

static struct timespec tmResolution;
int timeoutCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
        if (ulnow == 0) {
                return 0;
        }
        
        if (tmResolution.tv_nsec == 0) {
                clock_getres(CLOCK_MONOTONIC, &tmResolution);
        }
        struct timespec tp;
        clock_gettime(CLOCK_MONOTONIC, &tp);
        int64_t nNow = (int64_t)(tp.tv_sec * 1000000000ll + tp.tv_nsec / tmResolution.tv_nsec);
        
        KodoUploader * pUploader = (KodoUploader *)clientp;
        if (pUploader->nUlnowRecTime == 0) {
                pUploader->nLastUlnow = ulnow;
                pUploader->nUlnowRecTime = nNow;
                return 0;
        }
        
        int nDiff = (int)((nNow - pUploader->nUlnowRecTime) / 1000000000);
        if (nDiff > 0) {
                //printf("%d,==========dltotal:%lld dlnow:%lld ultotal:%lld ulnow-reculnow=%lld, now - lastrectime=%lld\n",
                //       pUploader->nLowSpeedCnt, dltotal, dlnow, ultotal, ulnow - pUploader->nLastUlnow, (nNow - pUploader->nUlnowRecTime)/1000000);
                if ((ulnow - pUploader->nLastUlnow) / nDiff < 1024) { //} && !pUploader->nIsFinished) {
                        pUploader->nLowSpeedCnt += nDiff;
                        if (pUploader->nLowSpeedCnt > 3) {
                                logerror("accumulate upload timeout:%d %d", pUploader->nLowSpeedCnt, nDiff);
                                return -1;
                        }
                }
                if (nDiff >= 10) {
                        logerror("upload timeout directly:%d", nDiff); 
                        return -1;
                } else if (nDiff >= 5) {
                        if (pUploader->nLowSpeedCnt > 1) {
                                logerror("half accumulate upload timeout:%d %d", pUploader->nLowSpeedCnt, nDiff);
                                return -1;
                        }
                        logwarn("accumulate2 upload timeout:%d %d", pUploader->nLowSpeedCnt, nDiff);
                        pUploader->nLowSpeedCnt = 2;

                }
                pUploader->nLastUlnow = ulnow;
                pUploader->nUlnowRecTime = nNow;
        }
        return 0;
}

static char * getErrorMsg(const char *_pJson, char *_pBuf, int _nBufLen)
{
        const char * pStart = _pJson;
        pStart = strstr(pStart, "\\\"error\\\"");
        printf("\\\"error\\\"");
        if (pStart == NULL)
                return NULL;
        pStart += strlen("\\\"error\\\"");

        while(*pStart != '"') {
                pStart++;
        }
        pStart++;

        const char * pEnd = strchr(pStart+1, '\\');
        if (pEnd == NULL)
                return NULL;
        int nLen = pEnd - pStart;
        if(nLen > _nBufLen - 1) {
                nLen = _nBufLen - 1;
        }
        memcpy(_pBuf, pStart, nLen);
        _pBuf[nLen] = 0;
        return _pBuf;
}

static const unsigned char pr2six[256] = {
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
        64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 63,
        64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

static void Base64Decode(char *bufplain, const char *bufcoded) {
        register const unsigned char *bufin;
        register unsigned char *bufout;
        register int nprbytes;
        
        bufin = (const unsigned char *) bufcoded;
        while (pr2six[*(bufin++)] <= 63);
        nprbytes = (bufin - (const unsigned char *) bufcoded) - 1;
        
        bufout = (unsigned char *) bufplain;
        bufin = (const unsigned char *) bufcoded;
        
        while (nprbytes > 4) {
                *(bufout++) = (unsigned char) (pr2six[*bufin] << 2 | pr2six[bufin[1]] >> 4);
                *(bufout++) = (unsigned char) (pr2six[bufin[1]] << 4 | pr2six[bufin[2]] >> 2);
                *(bufout++) = (unsigned char) (pr2six[bufin[2]] << 6 | pr2six[bufin[3]]);
                bufin += 4;
                nprbytes -= 4;
        }
        
        if (nprbytes > 1)
                *(bufout++) = (unsigned char) (pr2six[*bufin] << 2 | pr2six[bufin[1]] >> 4);
        if (nprbytes > 2)
                *(bufout++) = (unsigned char) (pr2six[bufin[1]] << 4 | pr2six[bufin[2]] >> 2);
        if (nprbytes > 3)
                *(bufout++) = (unsigned char) (pr2six[bufin[2]] << 6 | pr2six[bufin[3]]);
        
        *(bufout++) = '\0';
}

static int getExpireDays(char * pToken)
{
        char * pPolicy = strchr(pToken, ':');
        if (pPolicy == NULL) {
                return TK_ARG_ERROR;
        }
        pPolicy++;
        pPolicy = strchr(pPolicy, ':');
        if (pPolicy == NULL) {
                return TK_ARG_ERROR;
        }
        
        pPolicy++; //jump :
        int len = (strlen(pPolicy) + 2) * 3 / 4 + 1;
        char *pPlain = malloc(len);
        Base64Decode(pPlain, pPolicy);
        pPlain[len - 1] = 0;
        
        char *pExpireStart = strstr(pPlain, "\"deleteAfterDays\"");
        if (pExpireStart == NULL) {
                free(pPlain);
                return 0;
        }
        pExpireStart += strlen("\"deleteAfterDays\"");
        
        char days[10] = {0};
        int nStartFlag = 0;
        int nDaysLen = 0;
        char *pDaysStrat = NULL;
        while(1) {
                if (*pExpireStart >= 0x30 && *pExpireStart <= 0x39) {
                        if (nStartFlag == 0) {
                                pDaysStrat = pExpireStart;
                                nStartFlag = 1;
                        }
                        nDaysLen++;
                }else {
                        if (nStartFlag)
                                break;
                }
                pExpireStart++;
        }
        memcpy(days, pDaysStrat, nDaysLen);
        free(pPlain);
        return atoi(days);
}

#ifdef MULTI_SEG_TEST
static int newSegCount = 0;
#endif
static void * streamUpload(void *_pOpaque)
{
        KodoUploader * pUploader = (KodoUploader *)_pOpaque;
        
        char *uptoken = NULL;
        Qiniu_Client client;
        int canFreeToken = 0;

        uptoken = pUploader->uploadArg.pToken_;
        Qiniu_Client_InitNoAuth(&client, 1024);

        Qiniu_Io_PutRet putRet;
        Qiniu_Io_PutExtra putExtra;
        Qiniu_Zero(putExtra);
        //设置机房域名
        //Qiniu_Use_Zone_Beimei(Qiniu_False);
        //Qiniu_Use_Zone_Huabei(Qiniu_True);
        //Qiniu_Use_Zone_Huadong(Qiniu_True);
#ifdef DISABLE_OPENSSL
        Qiniu_Use_Zone_Huadong(Qiniu_False);
#else
        Qiniu_Use_Zone_Huadong(Qiniu_True);
#endif
        //Qiniu_Use_Zone_Huanan(Qiniu_True);
        
        //put extra
        //putExtra.upHost="http://nbxs-gate-up.qiniu.com";
        
        char key[128] = {0};
        
        // wait for first packet
        if (pUploader->nWaitFirstMutexLocked_ == WF_LOCKED) {
                pthread_mutex_lock(&pUploader->waitFirstMutex_);
                pthread_mutex_unlock(&pUploader->waitFirstMutex_);
        }
        if (pUploader->nWaitFirstMutexLocked_ != WF_FIRST) {
                goto END;
        }
        logdebug("upload start");
        
        int64_t curTime = GetCurrentNanosecond();
        // ts/uid/ua_id/yyyy/mm/dd/hh/mm/ss/mmm/fragment_start_ts/expiry.ts
        
        if (pUploader->uploadArg.nSegmentId_ == 0) {
                pUploader->uploadArg.nSegmentId_ = curTime;
        }
        pUploader->uploadArg.nLastUploadTsTime_ = curTime;
        if (pUploader->uploadArg.UploadArgUpadate) {
                pUploader->uploadArg.UploadArgUpadate(pUploader->uploadArg.pUploadArgKeeper_, &pUploader->uploadArg, curTime);
        }
        uint64_t nSegmentId = pUploader->uploadArg.nSegmentId_;
        
        int nDeleteAfterDays_ = getExpireDays(uptoken);
        memset(key, 0, sizeof(key));
        //ts/uaid/startts/fragment_start_ts/expiry.ts
        sprintf(key, "ts/%s/%lld/%lld/%d.ts", pUploader->uploadArg.pDeviceId_,
                curTime / 1000000, nSegmentId / 1000000, nDeleteAfterDays_);
#ifdef TK_STREAM_UPLOAD
        client.xferinfoData = _pOpaque;
        client.xferinfoCb = timeoutCallback;
        Qiniu_Error error = Qiniu_Io_PutStream(&client, &putRet, uptoken, key, pUploader, -1, getDataCallback, &putExtra);
#else
        Qiniu_Error error = Qiniu_Io_PutBuffer(&client, &putRet, uptoken, key, (const char*)pUploader->pTsData,
                                               pUploader->nTsDataLen, &putExtra);
#endif
        
#ifdef __ARM
        report_status( error.code, key );// add by liyq to record ts upload status
#endif
        if (error.code != 200) {
                pUploader->state = TK_UPLOAD_FAIL;
                if (error.code == 401) {
                        logerror("upload file :%s expsize:%d httpcode=%d errmsg=%s", key, pUploader->getDataBytes, error.code, Qiniu_Buffer_CStr(&client.b));
                } else if (error.code >= 500) {
                        const char * pFullErrMsg = Qiniu_Buffer_CStr(&client.b);
                        char errMsg[256];
                        char *pMsg = getErrorMsg(pFullErrMsg, errMsg, sizeof(errMsg));
                        if (pMsg) {
                                logerror("upload file :%s httpcode=%d errmsg={\"error\":\"%s\"}", key, error.code, pMsg);
                        }else {
                                logerror("upload file :%s httpcode=%d errmsg=%s", key, error.code,
                                         pFullErrMsg);
                        }
                } else {
			const char *pCurlErrMsg = curl_easy_strerror(error.code);
			if (pCurlErrMsg != NULL) {
                                logerror("upload file :%s expsize:%d errorcode=%d errmsg={\"error\":\"%s\"}", key, pUploader->getDataBytes, error.code, pCurlErrMsg);
			} else {
                                logerror("upload file :%s expsize:%d errorcode=%d errmsg={\"error\":\"unknown error\"}", key, pUploader->getDataBytes, error.code);
			}
                }
                //debug_log(&client, error);
        } else {
                pUploader->state = TK_UPLOAD_OK;
                logdebug("upload file size:(exp:%lld real:%lld) key:%s success",
                          pUploader->getDataBytes, pUploader->nLastUlnow, key);
        }
END:
        if (canFreeToken) {
                Qiniu_Free(uptoken);
        }
        Qiniu_Client_Cleanup(&client);

        return 0;
}

#ifdef TK_STREAM_UPLOAD
size_t getDataCallback(void* buffer, size_t size, size_t n, void* rptr)
{
        KodoUploader * pUploader = (KodoUploader *) rptr;
        int nPopLen = 0;
        nPopLen = pUploader->pQueue_->Pop(pUploader->pQueue_, buffer, size * n);
        if (nPopLen < 0) {
		if (nPopLen == TK_TIMEOUT) {
                        if (pUploader->nLastFrameTimestamp >= 0 &&  pUploader->nFirstFrameTimestamp >= 0) {
                                return 0;
                        }
                        logerror("first pop from queue timeout:%d %lld %lld", nPopLen, pUploader->nLastFrameTimestamp, pUploader->nFirstFrameTimestamp);
		}
                return CURL_READFUNC_ABORT;
        }
        if (nPopLen == 0) {
                if (IsProcStatusQuit()) {
                        return CURL_READFUNC_ABORT;
                }
                return 0;
        }

        int nTmp = 0;
        char *pBuf = (char *)buffer;
        while (size * n - nPopLen > 0) {
                nTmp = pUploader->pQueue_->Pop(pUploader->pQueue_, pBuf + nPopLen, size * n - nPopLen);
                if (nTmp == 0)
                        break;
                if (nTmp < 0) {
		        if (nTmp == TK_TIMEOUT) {
                                if (pUploader->nLastFrameTimestamp >= 0 &&  pUploader->nFirstFrameTimestamp >= 0) {
                                        goto RET;
                                }
                                logerror("next pop from queue timeout:%d %lld %lld", nTmp, pUploader->nLastFrameTimestamp, pUploader->nFirstFrameTimestamp);
                        }
                        return CURL_READFUNC_ABORT;
                }
                nPopLen += nTmp;
        }
        UploaderStatInfo info;
        pUploader->pQueue_->GetStatInfo(rptr, &info);
        //if (!info.nIsReadOnly) {
        //        pUploader->nIsFinished = 1;
        //}
RET:
        pUploader->getDataBytes += nPopLen;
        return nPopLen;
}

static int streamUploadStart(TsUploader * _pUploader)
{
        KodoUploader * pKodoUploader = (KodoUploader *)_pUploader;
        int ret = pthread_create(&pKodoUploader->workerId_, NULL, streamUpload, _pUploader);
        if (ret == 0) {
                pKodoUploader->isThreadStarted_ = 1;
                return 0;
        } else {
                logerror("start upload thread fail:%d", ret);
                return TK_THREAD_ERROR;
        }
}

static void streamUploadStop(TsUploader * _pUploader)
{
        KodoUploader * pKodoUploader = (KodoUploader *)_pUploader;
        if(pKodoUploader->nWaitFirstMutexLocked_ == WF_LOCKED) {
                pKodoUploader->nWaitFirstMutexLocked_ = WF_QUIT;
                pthread_mutex_unlock(&pKodoUploader->waitFirstMutex_);
        }
        pthread_mutex_lock(&pKodoUploader->waitFirstMutex_);
        pKodoUploader->nWaitFirstMutexLocked_ = WF_QUIT;
        pthread_mutex_unlock(&pKodoUploader->waitFirstMutex_);
        
        if (pKodoUploader->isThreadStarted_) {
                pKodoUploader->pQueue_->StopPush(pKodoUploader->pQueue_);
                pthread_join(pKodoUploader->workerId_, NULL);
                pKodoUploader->isThreadStarted_ = 0;
        }
        return;
}

static int streamPushData(TsUploader *pTsUploader, char * pData, int nDataLen)
{
        KodoUploader * pKodoUploader = (KodoUploader *)pTsUploader;
        
        int ret = pKodoUploader->pQueue_->Push(pKodoUploader->pQueue_, (char *)pData, nDataLen);
        if (pKodoUploader->nWaitFirstMutexLocked_ == WF_LOCKED) {
                pKodoUploader->nWaitFirstMutexLocked_ = WF_FIRST;
                pthread_mutex_unlock(&pKodoUploader->waitFirstMutex_);
        }
        return ret;
}

#else

static int memUploadStart(TsUploader * _pUploader)
{
        return 0;
}

static void memUploadStop(TsUploader * _pUploader)
{
        return;
}

static int memPushData(TsUploader *pTsUploader, char * pData, int nDataLen)
{
        KodoUploader * pKodoUploader = (KodoUploader *)pTsUploader;
        if (pKodoUploader->pTsData == NULL) {
                pKodoUploader->pTsData = malloc(pKodoUploader->nTsDataCap);
                pKodoUploader->nTsDataLen = 0;
        }
        if (pKodoUploader->nTsDataLen + nDataLen > pKodoUploader->nTsDataCap){
                char * tmp = malloc(pKodoUploader->nTsDataCap * 2);
                memcpy(tmp, pKodoUploader->pTsData, pKodoUploader->nTsDataLen);
                free(pKodoUploader->pTsData);
                pKodoUploader->pTsData = tmp;
                pKodoUploader->nTsDataCap *= 2;
                memcpy(tmp + pKodoUploader->nTsDataLen, pData, nDataLen);
                pKodoUploader->nTsDataLen += nDataLen;
                return nDataLen;
        }
        memcpy(pKodoUploader->pTsData + pKodoUploader->nTsDataLen, pData, nDataLen);
        pKodoUploader->nTsDataLen += nDataLen;
        return nDataLen;
}
#endif

static void getStatInfo(TsUploader *pTsUploader, UploaderStatInfo *_pStatInfo)
{
        KodoUploader * pKodoUploader = (KodoUploader *)pTsUploader;
#ifdef TK_STREAM_UPLOAD
        pKodoUploader->pQueue_->GetStatInfo(pKodoUploader->pQueue_, _pStatInfo);
#else
        _pStatInfo->nLen_ = 0;
        _pStatInfo->nPopDataBytes_ = pKodoUploader->nTsDataLen;
        _pStatInfo->nPopDataBytes_ = pKodoUploader->nTsDataLen;
#endif
        return;
}

void recordTimestamp(TsUploader *_pTsUploader, int64_t _nTimestamp)
{
        KodoUploader * pKodoUploader = (KodoUploader *)_pTsUploader;
        if (pKodoUploader->nFirstFrameTimestamp == -1) {
                pKodoUploader->nFirstFrameTimestamp = _nTimestamp;
                pKodoUploader->nLastFrameTimestamp = _nTimestamp;
        }
        pKodoUploader->nLastFrameTimestamp = _nTimestamp;
        return;
}

UploadState getUploaderState(TsUploader *_pTsUploader)
{
        KodoUploader * pKodoUploader = (KodoUploader *)_pTsUploader;
        return pKodoUploader->state;
}
        
int NewUploader(TsUploader ** _pUploader, UploadArg *_pArg, enum CircleQueuePolicy _policy, int _nMaxItemLen, int _nInitItemCount)
{
        KodoUploader * pKodoUploader = (KodoUploader *) malloc(sizeof(KodoUploader));
        if (pKodoUploader == NULL) {
                return TK_NO_MEMORY;
        }

        memset(pKodoUploader, 0, sizeof(KodoUploader));
        
        int ret = pthread_mutex_init(&pKodoUploader->waitFirstMutex_, NULL);
        if (ret != 0){
                free(pKodoUploader);
                return TK_MUTEX_ERROR;
        }
        pthread_mutex_lock(&pKodoUploader->waitFirstMutex_);
        pKodoUploader->nWaitFirstMutexLocked_ = WF_LOCKED;
#ifdef TK_STREAM_UPLOAD
        ret = NewCircleQueue(&pKodoUploader->pQueue_, 0, _policy, _nMaxItemLen, _nInitItemCount);
        if (ret != 0) {
                free(pKodoUploader);
                return ret;
        }
#else
        pKodoUploader->nTsDataCap = 1024 * 1024;
#endif
        pKodoUploader->nFirstFrameTimestamp = -1;
        pKodoUploader->nLastFrameTimestamp = -1;
        pKodoUploader->uploadArg = *_pArg;
#ifdef TK_STREAM_UPLOAD
        pKodoUploader->uploader.UploadStart = streamUploadStart;
        pKodoUploader->uploader.UploadStop = streamUploadStop;
        pKodoUploader->uploader.Push = streamPushData;
#else
        pKodoUploader->uploader.UploadStart = memUploadStart;
        pKodoUploader->uploader.UploadStop = memUploadStop;
        pKodoUploader->uploader.Push = memPushData;
#endif
        pKodoUploader->uploader.GetStatInfo = getStatInfo;
        pKodoUploader->uploader.RecordTimestamp = recordTimestamp;
        pKodoUploader->uploader.GetUploaderState = getUploaderState;
        
        *_pUploader = (TsUploader*)pKodoUploader;
        
        return 0;
}

void DestroyUploader(TsUploader ** _pUploader)
{
        KodoUploader * pKodoUploader = (KodoUploader *)(*_pUploader);
        
        pthread_mutex_destroy(&pKodoUploader->waitFirstMutex_);
#ifdef TK_STREAM_UPLOAD
        if (pKodoUploader->isThreadStarted_) {
                pthread_join(pKodoUploader->workerId_, NULL);
        }
        DestroyQueue(&pKodoUploader->pQueue_);
#else
        free(pKodoUploader->pTsData);
#endif
        
        free(pKodoUploader);
        * _pUploader = NULL;
        return;
}
