/*
	Copyleft (c) 2013-2015 EasyDarwin.ORG.  All rights reserved.
	Github: https://github.com/EasyDarwin
	WEChat: EasyDarwin
	Website: http://www.EasyDarwin.org
*/
/*
    File:       HTTPSession.cpp
    Contains:   实现对服务单元每一个Session会话的网络报文处理
*/

#undef COMMON_UTILITIES_LIB
#include "HTTPSession.h"
#include "QTSServerInterface.h"
#include "OSArrayObjectDeleter.h"
#include "OSMemory.h"
#include "QTSSMemoryDeleter.h"
#include "QueryParamList.h"

#include "EasyProtocolDef.h"
#include "EasyProtocol.h"

using namespace EasyDarwin::Protocol;
using namespace std;

#if __FreeBSD__ || __hpux__	
    #include <unistd.h>
#endif

#include <errno.h>

#if __solaris__ || __linux__ || __sgi__	|| __hpux__
    #include <crypt.h>
#endif

static StrPtrLen	sServiceStr("EasyDarwin");
static StrPtrLen	sEasyHLSModule("EasyHLSModule");
#define	QUERY_STREAM_NAME		"name"
#define QUERY_STREAM_URL		"url"
#define QUERY_STREAM_CMD		"cmd"
#define QUERY_STREAM_CMD_START	"start"
#define QUERY_STREAM_CMD_STOP	"stop"


HTTPSession::HTTPSession( )
: HTTPSessionInterface(),
  fRequest(NULL),
  fReadMutex(),
  fCurrentModule(0),
  fState(kReadingFirstRequest)
{
    this->SetTaskName("HTTPSession");
    
	//在全局服务对象中Session数增长一个
    QTSServerInterface::GetServer()->AlterCurrentRTSPSessionCount(1);

    // Setup the QTSS param block, as none of these fields will change through the course of this session.
    fRoleParams.rtspRequestParams.inRTSPSession = this;
    fRoleParams.rtspRequestParams.inRTSPRequest = NULL;
    fRoleParams.rtspRequestParams.inClientSession = NULL;
    
    fModuleState.curModule = NULL;
    fModuleState.curTask = this;
    fModuleState.curRole = 0;
    fModuleState.globalLockRequested = false;
}

HTTPSession::~HTTPSession()
{
	char remoteAddress[20] = {0};
	StrPtrLen theIPAddressStr(remoteAddress,sizeof(remoteAddress));
	QTSS_GetValue(this, easyHTTPSesRemoteAddrStr, 0, (void*)theIPAddressStr.Ptr, &theIPAddressStr.Len);

	char msgStr[2048] = { 0 };
	qtss_snprintf(msgStr, sizeof(msgStr), "session offline from ip[%s]",remoteAddress);
	QTSServerInterface::LogError(qtssMessageVerbosity, msgStr);
    // Invoke the session closing modules
    QTSS_RoleParams theParams;
    theParams.rtspSessionClosingParams.inRTSPSession = this;
    
    //会话断开时，调用模块进行一些停止的工作
    for (UInt32 x = 0; x < QTSServerInterface::GetNumModulesInRole(QTSSModule::kRTSPSessionClosingRole); x++)
        (void)QTSServerInterface::GetModule(QTSSModule::kRTSPSessionClosingRole, x)->CallDispatch(QTSS_RTSPSessionClosing_Role, &theParams);

    fLiveSession = false; //used in Clean up request to remove the RTP session.
    this->CleanupRequest();// Make sure that all our objects are deleted
    //if (fSessionType == qtssServiceSession)
    //    QTSServerInterface::GetServer()->AlterCurrentServiceSessionCount(-1);

}

/*!
	\brief 事件由HTTPSession Task进行处理，大多数为网络报文处理事件 
	\param 
	\return 处理完成返回0,断开Session返回-1
	\ingroup 
	\see 
*/
SInt64 HTTPSession::Run()
{
	//获取事件类型
    EventFlags events = this->GetEvents();
    QTSS_Error err = QTSS_NoErr;
    QTSSModule* theModule = NULL;
    UInt32 numModules = 0;
    // Some callbacks look for this struct in the thread object
    OSThreadDataSetter theSetter(&fModuleState, NULL);
        
    //超时事件或者Kill事件，进入释放流程：清理 & 返回-1
    if (events & Task::kKillEvent)
        fLiveSession = false;

	if(events & Task::kTimeoutEvent)
	{
		//客户端Session超时，暂时不处理 
		char msgStr[512];
		qtss_snprintf(msgStr, sizeof(msgStr), "session timeout,release session\n");
		QTSServerInterface::LogError(qtssMessageVerbosity, msgStr);
		return -1;
	}

	//正常事件处理流程
    while (this->IsLiveSession())
    {
        //报文处理以状态机的形式，可以方便多次处理同一个消息
        switch (fState)
        {
            case kReadingFirstRequest://首次对Socket进行读取
            {
                if ((err = fInputStream.ReadRequest()) == QTSS_NoErr)
                {
					//如果RequestStream返回QTSS_NoErr，就表示已经读取了目前所到达的网络数据
					//但，还不能构成一个整体报文，还要继续等待读取...
                    fInputSocketP->RequestEvent(EV_RE);
                    return 0;
                }
                
                if ((err != QTSS_RequestArrived) && (err != E2BIG))
                {
                    // Any other error implies that the client has gone away. At this point,
                    // we can't have 2 sockets, so we don't need to do the "half closed" check
                    // we do below
                    Assert(err > 0); 
                    Assert(!this->IsLiveSession());
                    break;
                }

                if ((err == QTSS_RequestArrived) || (err == E2BIG))
                    fState = kHaveCompleteMessage;
            }
            continue;            
            case kReadingRequest://读取请求报文
            {
				//读取锁，已经在处理一个报文包时，不进行新网络报文的读取和处理
                OSMutexLocker readMutexLocker(&fReadMutex);

				//网络请求报文存储在fInputStream中
                if ((err = fInputStream.ReadRequest()) == QTSS_NoErr)
                {
					//如果RequestStream返回QTSS_NoErr，就表示已经读取了目前所到达的网络数据
					//但，还不能构成一个整体报文，还要继续等待读取...
                    fInputSocketP->RequestEvent(EV_RE);
                    return 0;
                }
                
                if ((err != QTSS_RequestArrived) && (err != E2BIG) && (err != QTSS_BadArgument))
                {
                    //Any other error implies that the input connection has gone away.
                    // We should only kill the whole session if we aren't doing HTTP.
                    // (If we are doing HTTP, the POST connection can go away)
                    Assert(err > 0);
                    if (fOutputSocketP->IsConnected())
                    {
                        // If we've gotten here, this must be an HTTP session with
                        // a dead input connection. If that's the case, we should
                        // clean up immediately so as to not have an open socket
                        // needlessly lingering around, taking up space.
                        Assert(fOutputSocketP != fInputSocketP);
                        Assert(!fInputSocketP->IsConnected());
                        fInputSocketP->Cleanup();
                        return 0;
                    }
                    else
                    {
                        Assert(!this->IsLiveSession());
                        break;
                    }
                }
                fState = kHaveCompleteMessage;
            }
            case kHaveCompleteMessage://读取到完整的请求报文
            {
                Assert( fInputStream.GetRequestBuffer() );
                
                Assert(fRequest == NULL);
				//根据具体请求报文构造HTTPRequest请求类
				fRequest = NEW HTTPRequest(&sServiceStr, fInputStream.GetRequestBuffer());

				//在这里，我们已经读取了一个完整的Request，并准备进行请求的处理，直到响应报文发出
				//在此过程中，此Session的Socket不进行任何网络数据的读/写；
                fReadMutex.Lock();
                fSessionMutex.Lock();
                
                //清空发送缓冲区
                fOutputStream.ResetBytesWritten();
                
				//网络请求超过了缓冲区，返回Bad Request
                if (err == E2BIG)
                {
					//返回HTTP报文，错误码408
                    //(void)QTSSModuleUtils::SendErrorResponse(fRequest, qtssClientBadRequest, qtssMsgRequestTooLong);
                    fState = kSendingResponse;
                    break;
                }
                // Check for a corrupt base64 error, return an error
                if (err == QTSS_BadArgument)
                {
					//返回HTTP报文，错误码408
                    //(void)QTSSModuleUtils::SendErrorResponse(fRequest, qtssClientBadRequest, qtssMsgBadBase64);
                    fState = kSendingResponse;
                    break;
                }

                Assert(err == QTSS_RequestArrived);
                fState = kFilteringRequest;
            }
            
            case kFilteringRequest:
            {
                //刷新Session保活时间
                fTimeoutTask.RefreshTimeout();

				//对请求报文进行解析
				QTSS_Error theErr = SetupRequest();
				//当SetupRequest步骤未读取到完整的网络报文，需要进行等待
				if(theErr == QTSS_WouldBlock)
				{
					this->ForceSameThread();
					fInputSocketP->RequestEvent(EV_RE);
					// We are holding mutexes, so we need to force
					// the same thread to be used for next Run()
                    return 0;//返回0表示有事件才进行通知，返回>0表示规定事件后调用Run

				}
                
                //每一步都检测响应报文是否已完成，完成则直接进行回复响应
                if (fOutputStream.GetBytesWritten() > 0)
                {
                    fState = kSendingResponse;
                    break;
                }

                fState = kPreprocessingRequest;
                break;
            }
                       
            case kPreprocessingRequest:
            {
                //请求预处理过程
				//TODO:报文处理过程
                fState = kCleaningUp;
				break;
            }

            case kProcessingRequest:
            {
                if (fOutputStream.GetBytesWritten() == 0)
                {
					// 响应报文还没有形成
					//QTSSModuleUtils::SendErrorResponse(fRequest, qtssServerInternal, qtssMsgNoModuleForRequest);
					fState = kCleaningUp;
					break;
                }

                fState = kSendingResponse;
            }
            case kSendingResponse:
            {
                //响应报文发送，确保完全发送
                Assert(fRequest != NULL);

				//发送响应报文
                err = fOutputStream.Flush();
                
                if (err == EAGAIN)
                {
                    // If we get this error, we are currently flow-controlled and should
                    // wait for the socket to become writeable again
					//如果收到Socket EAGAIN错误，那么我们需要等Socket再次可写的时候再调用发送
                    fSocket.RequestEvent(EV_WR);
                    this->ForceSameThread();
					// We are holding mutexes, so we need to force
					// the same thread to be used for next Run()
                    return 0;
                }
                else if (err != QTSS_NoErr)
                {
                    // Any other error means that the client has disconnected, right?
                    Assert(!this->IsLiveSession());
                    break;
                }
            
                fState = kCleaningUp;
            }
            
            case kCleaningUp:
            {
                // Cleaning up consists of making sure we've read all the incoming Request Body
                // data off of the socket
                if (this->GetRemainingReqBodyLen() > 0)
                {
                    err = this->DumpRequestData();
                    
                    if (err == EAGAIN)
                    {
                        fInputSocketP->RequestEvent(EV_RE);
                        this->ForceSameThread();    // We are holding mutexes, so we need to force
                                                    // the same thread to be used for next Run()
                        return 0;
                    }
                }

				//一次请求的读取、处理、响应过程完整，等待下一次网络报文！
                this->CleanupRequest();
                fState = kReadingRequest;
            }
        }
    } 

	//清空Session占用的所有资源
    this->CleanupRequest();

    //Session引用数为0，返回-1后，系统会将此Session删除
    if (fObjectHolders == 0)
        return -1;

	//如果流程走到这里，Session实际已经无效了，应该被删除，但没有，因为还有其他地方引用了Session对象
    return 0;
}

/*
	发送HTTP+json报文，决定是否关闭当前Session
	HTTP部分构造，json部分由函数传递
*/
QTSS_Error HTTPSession::SendHTTPPacket(StrPtrLen* contentXML, Bool16 connectionClose, Bool16 decrement)
{
	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&sServiceStr);
	httpAck.CreateResponseHeader(contentXML->Len?httpOK:httpNotImplemented);
	if (contentXML->Len)
		httpAck.AppendContentLengthHeader(contentXML->Len);

	if(connectionClose)
		httpAck.AppendConnectionCloseHeader();

	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteResponseHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);
	
	RTSPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (contentXML->Len > 0) 
		pOutputStream->Put(contentXML->Ptr, contentXML->Len);

	if (pOutputStream->GetBytesWritten() != 0)
	{
		pOutputStream->Flush();
	}

	//将对HTTPSession的引用减少一
	if(fObjectHolders && decrement)
		DecrementObjectHolderCount();

	if(connectionClose)
		this->Signal(Task::kKillEvent);

	return QTSS_NoErr;
}

/*
	处理HTTP请求报文
*/
QTSS_Error HTTPSession::SetupRequest()
{
    //解析请求报文
    QTSS_Error theErr = fRequest->Parse();
    if (theErr != QTSS_NoErr)
        return QTSS_BadArgument;

	const char* location = NULL;
    QTSS_RTSPStatusCode statusCode = qtssSuccessOK;
    char *body = NULL;
    UInt32 bodySizeBytes = 0;

	if(fRequest->GetRequestPath())
	{
		StrPtrLen theFullPath(fRequest->GetRequestPath());

		if (theFullPath.Equal(sEasyHLSModule))
		{
			return ExecNetMsgEasyHLSModuleReq(fRequest->GetQueryString(), NULL);
		}
	}

	//获取具体Content json数据部分

	//1、获取json部分长度
	StrPtrLen* lengthPtr = fRequest->GetHeaderValue(httpContentLengthHeader);
	StringParser theContentLenParser(lengthPtr);
    theContentLenParser.ConsumeWhitespace();
    UInt32 content_length = theContentLenParser.ConsumeInteger(NULL);

    if (content_length)
	{	
		qtss_printf("HTTPSession read content-length:%d \n", content_length);
		// Check for the existence of 2 attributes in the request: a pointer to our buffer for
		// the request body, and the current offset in that buffer. If these attributes exist,
		// then we've already been here for this request. If they don't exist, add them.
		UInt32 theBufferOffset = 0;
		char* theRequestBody = NULL;
		 UInt32 theLen = 0;
		theLen = sizeof(theRequestBody);
		theErr = QTSS_GetValue(this, easyHTTPSesContentBody, 0, &theRequestBody, &theLen);

		if (theErr != QTSS_NoErr)
		{
			// First time we've been here for this request. Create a buffer for the content body and
			// shove it in the request.
			theRequestBody = NEW char[content_length + 1];
			memset(theRequestBody,0,content_length + 1);
			theLen = sizeof(theRequestBody);
			theErr = QTSS_SetValue(this, easyHTTPSesContentBody, 0, &theRequestBody, theLen);// SetValue creates an internal copy.
			Assert(theErr == QTSS_NoErr);
	        
			// Also store the offset in the buffer
			theLen = sizeof(theBufferOffset);
			theErr = QTSS_SetValue(this, easyHTTPSesContentBodyOffset, 0, &theBufferOffset, theLen);
			Assert(theErr == QTSS_NoErr);
		}
	    
		theLen = sizeof(theBufferOffset);
		theErr = QTSS_GetValue(this, easyHTTPSesContentBodyOffset, 0, &theBufferOffset, &theLen);

		// We have our buffer and offset. Read the data.
		//theErr = QTSS_Read(this, theRequestBody + theBufferOffset, content_length - theBufferOffset, &theLen);
		theErr = fInputStream.Read(theRequestBody + theBufferOffset, content_length - theBufferOffset, &theLen);
		Assert(theErr != QTSS_BadArgument);

		if (theErr == QTSS_RequestFailed)
		{
			OSCharArrayDeleter charArrayPathDeleter(theRequestBody);
			//
			// NEED TO RETURN HTTP ERROR RESPONSE
			return QTSS_RequestFailed;
		}
	    
		qtss_printf("Add Len:%d \n", theLen);
		if ((theErr == QTSS_WouldBlock) || (theLen < ( content_length - theBufferOffset)))
		{
			//
			// Update our offset in the buffer
			theBufferOffset += theLen;
			(void)QTSS_SetValue(this, easyHTTPSesContentBodyOffset, 0, &theBufferOffset, sizeof(theBufferOffset));
			// The entire content body hasn't arrived yet. Request a read event and wait for it.
	       
			Assert(theErr == QTSS_NoErr);
			return QTSS_WouldBlock;
		}

		Assert(theErr == QTSS_NoErr);
	    
		OSCharArrayDeleter charArrayPathDeleter(theRequestBody);

		////报文处理，不进入队列
		//EasyDarwin::Protocol::EasyProtocol protocol(theRequestBody);
		//int nNetMsg = protocol.GetMessageType();

		//switch (nNetMsg)
		//{
		//	case MSG_DEV_CMS_REGISTER_REQ:
		//		ExecNetMsgDevRegisterReq(theRequestBody);
		//		break;
		//	case MSG_NGX_CMS_NEED_STREAM_REQ:
		//		ExecNetMsgNgxStreamReq(theRequestBody);
		//		break;
		//	default:
		//		ExecNetMsgDefaultReqHandler(theRequestBody);
		//		break;
		//}
		
		UInt32 offset = 0;
		(void)QTSS_SetValue(this, easyHTTPSesContentBodyOffset, 0, &offset, sizeof(offset));
		char* content = NULL;
		(void)QTSS_SetValue(this, easyHTTPSesContentBody, 0, &content, 0);

	}

	qtss_printf("get complete http msg:%s QueryString:%s \n", fRequest->GetRequestPath(), fRequest->GetQueryString());

	return QTSS_NoErr;
}

void HTTPSession::CleanupRequest()
{
    if (fRequest != NULL)
    {
        // NULL out any references to the current request
        delete fRequest;
        fRequest = NULL;
        fRoleParams.rtspRequestParams.inRTSPRequest = NULL;
        fRoleParams.rtspRequestParams.inRTSPHeaders = NULL;
    }
    
    fSessionMutex.Unlock();
    fReadMutex.Unlock();
    
    // Clear out our last value for request body length before moving onto the next request
    this->SetRequestBodyLength(-1);
}

Bool16 HTTPSession::OverMaxConnections(UInt32 buffer)
{
    QTSServerInterface* theServer = QTSServerInterface::GetServer();
    SInt32 maxConns = theServer->GetPrefs()->GetMaxConnections();
    Bool16 overLimit = false;
    
    if (maxConns > -1) // limit connections
    { 
        UInt32 maxConnections = (UInt32) maxConns + buffer;
        if  ( theServer->GetNumRTSPSessions() > maxConnections ) 
        {
            overLimit = true;          
        }
    }
    return overLimit;
}


QTSS_Error HTTPSession::DumpRequestData()
{
    char theDumpBuffer[EASY_REQUEST_BUFFER_SIZE_LEN];
    
    QTSS_Error theErr = QTSS_NoErr;
    while (theErr == QTSS_NoErr)
        theErr = this->Read(theDumpBuffer, EASY_REQUEST_BUFFER_SIZE_LEN, NULL);
        
    return theErr;
}

QTSS_Error HTTPSession::ExecNetMsgEasyHLSModuleReq(char* queryString, char* json)
{	
	QTSS_Error theErr = QTSS_NoErr;
       
	char* hlsURL = NEW char[QTSS_MAX_URL_LENGTH];
    QTSSCharArrayDeleter theHLSURLDeleter(hlsURL);
	do{
		StrPtrLen theQueryString(queryString);

		QueryParamList parList(queryString);

		const char* sName = parList.DoFindCGIValueForParam(QUERY_STREAM_NAME);
		if(sName == NULL)
		{
			theErr = QTSS_Unimplemented;
			break;
		}

		const char* sURL = parList.DoFindCGIValueForParam(QUERY_STREAM_URL);
		const char* sCMD = parList.DoFindCGIValueForParam(QUERY_STREAM_CMD);

		bool bStop = false;
		if(sCMD)
		{
			if(::strcmp(sCMD,QUERY_STREAM_CMD_STOP) == 0)
				bStop = true;
		}

		if(bStop)
		{
			Easy_StopHLSSession(sName);
		}
		else
		{
			if(sURL == NULL)
			{
				theErr = QTSS_Unimplemented;
				break;
			}

			Easy_StartHLSSession(sName, sURL, hlsURL);
		}
	}while(0);

	StrPtrLen msgJson(hlsURL);
	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&sServiceStr);
	httpAck.CreateResponseHeader(msgJson.Len?httpOK:httpNotImplemented);
	if (msgJson.Len)
		httpAck.AppendContentLengthHeader(msgJson.Len);

	//响应完成后断开连接
	httpAck.AppendConnectionCloseHeader();

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteResponseHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);
	
	RTSPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (msgJson.Len > 0) 
		pOutputStream->Put(msgJson.Ptr, msgJson.Len);

	return QTSS_NoErr;
}


//QTSS_Error HTTPSession::ExecNetMsgNgxStreamReq(const char* json)
//{
//	QTSS_Error theErr = QTSS_NoErr;
//
//	//MSG_NGX_CMS_NEED_STREAM_REQ请求设备直播消息解析
//	EasyDarwin::Protocol::EasyProtocol req(json);
//
//	//获取设备SN序列号
//	std::string strDeviceSN = req.GetBodyValue(EASYDARWIN_TAG_DEVICE_SERIAL);
//
//	//这里对错误报文没有进行Response回复，实际是需要进行处理的
//	if(strDeviceSN.length()<=0) return QTSS_BadArgument;
//
//	printf("MSG_NGX_CMS_NEED_STREAM_REQ(%s)\n", strDeviceSN.c_str());
//
//	OSRefTable* devMap = QTSServerInterface::GetServer()->GetDeviceSessionMap();
//	OSRef* theDevRef = NULL;
//
//	do
//	{
//		//从设备列表MAP中查找设备
//		char strSerial[EASY_MAX_SERIAL_LENGTH] = { 0 };
//		qtss_sprintf(strSerial,"%s",strDeviceSN.c_str());
//
//		StrPtrLen devSerialPtr(strSerial);
//
//		// Device RefCount++
//		theDevRef = devMap->Resolve(&devSerialPtr);
//
//		if (theDevRef == NULL)
//		{
//			//未查找到设备，返回404
//			theErr = QTSS_ValueNotFound;
//			break;
//		}
//
//		//构造MSG_CMS_DEV_STREAM_START_REQ请求设备码流消息
//		EasyDarwin::Protocol::EasyProtocol streamingREQ(MSG_CMS_DEV_STREAM_START_REQ);
//		//Header
//		streamingREQ.SetHeaderValue(EASYDARWIN_TAG_VERSION, EASYDARWIN_PROTOCOL_VERSION);
//		streamingREQ.SetHeaderValue(EASYDARWIN_TAG_CSEQ, "1");	
//		//Body
//		streamingREQ.SetBodyValue(EASYDARWIN_TAG_STREAM_ID, EASYDARWIN_PROTOCOL_STREAM_MAIN);
//		streamingREQ.SetBodyValue(EASYDARWIN_TAG_PROTOCOL, "RTSP");
//		streamingREQ.SetBodyValue("DssIP","www.easydarwin.org");
//		streamingREQ.SetBodyValue("DssPort","554");
//	
//		string buffer = streamingREQ.GetMsg();
//		//发送MSG_CMS_DEV_STREAM_START_REQ报文到设备
//		QTSS_SendHTTPPacket(theDevRef->GetObject(), (char*)buffer.c_str(), ::strlen(buffer.c_str()), false, false);
//
//	}while(0);
//	
//	//Device RefCount--
//	if(theDevRef)
//		devMap->Release(theDevRef);
//
//	//构造MSG_NGX_CMS_NEED_STREAM_RSP请求直播消息响应报文
//	EasyDarwin::Protocol::EasyProtocol rsp(MSG_NGX_CMS_NEED_STREAM_RSP);
//	rsp.SetHeaderValue(EASYDARWIN_TAG_VERSION, EASYDARWIN_PROTOCOL_VERSION);
//	rsp.SetHeaderValue(EASYDARWIN_TAG_ERROR_NUM, "200");//EASYDARWIN_ERROR_SUCCESS_OK
//	rsp.SetHeaderValue(EASYDARWIN_TAG_ERROR_STRING, EasyProtocol::GetErrorString(EASYDARWIN_ERROR_SUCCESS_OK).c_str());
//	rsp.SetBodyValue("DssIP","www.easydarwin.org");
//	rsp.SetBodyValue("DssPort","554");
//
//	string msg = rsp.GetMsg();
//
//	StrPtrLen msgJson((char*)msg.c_str());
//
//	//构造响应报文(HTTP头)
//	HTTPRequest httpAck(&sServiceStr);
//	httpAck.CreateResponseHeader(msgJson.Len?httpOK:httpNotImplemented);
//	if (msgJson.Len)
//		httpAck.AppendContentLengthHeader(msgJson.Len);
//
//	//响应完成后断开连接
//	httpAck.AppendConnectionCloseHeader();
//
//	//Push MSG to OutputBuffer
//	char respHeader[2048] = { 0 };
//	StrPtrLen* ackPtr = httpAck.GetCompleteResponseHeader();
//	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);
//	
//	RTSPResponseStream *pOutputStream = GetOutputStream();
//	pOutputStream->Put(respHeader);
//	if (msgJson.Len > 0) 
//		pOutputStream->Put(msgJson.Ptr, msgJson.Len);
//	
//	return QTSS_NoErr;
//}
//
//QTSS_Error HTTPSession::ExecNetMsgDefaultReqHandler(const char* json)
//{
//	return QTSS_NoErr;
//}
//
//QTSS_Error HTTPSession::ExecNetMsgSnapUpdateReq(const char* json)
//{
//	QTSS_Error theErr = QTSS_NoErr;
//	bool close = false;
//	EasyDarwinDeviceSnapUpdateReq parse(json);
//	
//	string image;
//	parse.GetImageData(image);
//
//	const char* inSnapJpg = image.c_str();
//
//	if(!fAuthenticated) return QTSS_NoErr;
//
//	//先对数据进行Base64解码
//	int base64DataLen = Base64decode_len(inSnapJpg);
//	char* snapDecodedBuffer = (char*)malloc(base64DataLen);
//	::memset(snapDecodedBuffer, 0, base64DataLen);
//	//对数据进行Base64编码
//	int jpgLen = ::Base64decode(snapDecodedBuffer, inSnapJpg); 
//
//	char jpgDir[512] = { 0 };
//	qtss_sprintf(jpgDir,"%s%s", QTSServerInterface::GetServer()->GetPrefs()->GetSnapLocalPath() ,fSerial);
//	OS::RecursiveMakeDir(jpgDir);
//
//	char jpgPath[512] = { 0 };
//	string jpgFileName = EasyUtil::GetUUID();
//	qtss_sprintf(jpgPath,"%s/%s.jpg", jpgDir, jpgFileName.c_str());
//
//	FILE* fSnap = ::fopen(jpgPath, "wb");
//	fwrite(snapDecodedBuffer, 1, jpgLen, fSnap);
//	::fclose(fSnap);
//	free(snapDecodedBuffer);
//
//	qtss_sprintf(fDeviceSnap, "%s%s/%s.jpg",QTSServerInterface::GetServer()->GetPrefs()->GetSnapWebPath(), fSerial, jpgFileName.c_str());
//	return QTSS_NoErr;
//}
//
//QTSS_Error HTTPSession::ExecNetMsgGetDeviceListReq()
//{
//	QTSS_Error theErr = QTSS_NoErr;
//
//	printf("Get Device List! \n");
//
//	OSRefTable* devMap = QTSServerInterface::GetServer()->GetDeviceSessionMap();
//	if(devMap->GetNumRefsInTable() == 0 )
//		return QTSS_NoErr;
//
//	EasyDarwinDeviceListRsp req;
//	req.SetHeaderValue(EASYDARWIN_TAG_VERSION, "1.0");
//	req.SetHeaderValue(EASYDARWIN_TAG_TERMINAL_TYPE, EasyProtocol::GetTerminalTypeString(EASYDARWIN_TERMINAL_TYPE_CAMERA).c_str());
//	req.SetHeaderValue(EASYDARWIN_TAG_CSEQ, "1");	
//	char count[16] = { 0 };
//	sprintf(count,"%d", devMap->GetNumRefsInTable());
//	req.SetBodyValue("DeviceCount", count );
//
//	OSRef* theDevRef = NULL;
//
//	OSMutexLocker locker(devMap->GetMutex());
//    for (OSRefHashTableIter theIter(devMap->GetHashTable()); !theIter.IsDone(); theIter.Next())
//    {
//        OSRef* theRef = theIter.GetCurrent();
//        HTTPSession* theSession = (HTTPSession*)theRef->GetObject();
//
//		EasyDarwinDevice device;
//		device.DeviceName = string(theSession->GetDeviceSerial());
//		device.DeviceSerial = string(theSession->GetDeviceSerial());
//		device.DeviceSnap = string(theSession->GetDeviceSnap());
//		req.AddDevice(device);
//    }   
//
//	string msg = req.GetMsg();
//
//	printf(msg.c_str());
//
//	StrPtrLen msgJson((char*)msg.c_str());
//
//	//构造响应报文(HTTP头)
//	HTTPRequest httpAck(&sServiceStr);
//	httpAck.CreateResponseHeader(msgJson.Len?httpOK:httpNotImplemented);
//	if (msgJson.Len)
//		httpAck.AppendContentLengthHeader(msgJson.Len);
//
//	//响应完成后断开连接
//	httpAck.AppendConnectionCloseHeader();
//
//	//Push MSG to OutputBuffer
//	char respHeader[2048] = { 0 };
//	StrPtrLen* ackPtr = httpAck.GetCompleteResponseHeader();
//	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);
//	
//	RTSPResponseStream *pOutputStream = GetOutputStream();
//	pOutputStream->Put(respHeader);
//	if (msgJson.Len > 0) 
//		pOutputStream->Put(msgJson.Ptr, msgJson.Len);
//	
//	return QTSS_NoErr;
//}