/*
 * Copyright 2010-2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#include "S3ObjectTransport.h"
#include "CanaryApp.h"
#include "MetricsPublisher.h"

#include <aws/common/thread.h>
#include <aws/crt/Api.h>
#include <aws/crt/http/HttpConnectionManager.h>
#include <aws/crt/http/HttpRequestResponse.h>
#include <aws/crt/io/Stream.h>
#include <aws/io/stream.h>
#include <inttypes.h>
#include <iostream>

#if defined(_WIN32)
#    undef min
#endif

using namespace Aws::Crt;

namespace
{
    const uint32_t MaxStreams = 500;
    const uint32_t TransfersPerAddress = 10;
    const int32_t S3GetObjectResponseStatus_PartialContent = 206;
} // namespace

S3ObjectTransport::S3ObjectTransport(CanaryApp &canaryApp, const Aws::Crt::String &bucket)
    : m_canaryApp(canaryApp), m_bucketName(bucket), m_connManagersUseCount(0), m_activeRequestsCount(0),
      m_uploadProcessor(canaryApp, canaryApp.GetEventLoopGroup(), MaxStreams),
      m_downloadProcessor(canaryApp, canaryApp.GetEventLoopGroup(), MaxStreams)
{
    m_endpoint = m_bucketName + ".s3." + m_canaryApp.GetOptions().region.c_str() + ".amazonaws.com";

    m_hostHeader.name = ByteCursorFromCString("host");
    m_hostHeader.value = ByteCursorFromCString(m_endpoint.c_str());

    m_contentTypeHeader.name = ByteCursorFromCString("content-type");
    m_contentTypeHeader.value = ByteCursorFromCString("text/plain");
}

size_t S3ObjectTransport::GetOpenConnectionCount()
{
    return m_activeRequestsCount;
}

void S3ObjectTransport::EmitS3AddressCountMetric(uint32_t addressCount)
{
    AWS_LOGF_INFO(AWS_LS_CRT_CPP_CANARY, "Emitting S3 Address Count Metric: %d", addressCount);

    Metric s3AddressCountMetric(MetricName::S3AddressCount, MetricUnit::Count, (double)addressCount);
    m_canaryApp.GetMetricsPublisher()->AddDataPoint(s3AddressCountMetric);
}

void S3ObjectTransport::WarmDNSCache(uint32_t numTransfers)
{
    uint32_t desiredNumberOfAddresses = numTransfers / TransfersPerAddress;

    if ((numTransfers % TransfersPerAddress) > 0)
    {
        ++desiredNumberOfAddresses;
    }

    AWS_LOGF_INFO(
        AWS_LS_CRT_CPP_CANARY,
        "Warming DNS cache: getting %d addresses for endpoint %s",
        desiredNumberOfAddresses,
        m_endpoint.c_str());

    m_canaryApp.GetDefaultHostResolver().ResolveHost(
        m_endpoint, [](Io::HostResolver &, const Vector<Io::HostAddress> &, int) {});

    uint32_t numAddresses =
        m_canaryApp.GetDefaultHostResolver().GetHostAddressCount(m_endpoint, AWS_GET_HOST_ADDRESS_COUNT_RECORD_TYPE_A);

    EmitS3AddressCountMetric(numAddresses);

    while (numAddresses < desiredNumberOfAddresses)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        numAddresses = m_canaryApp.GetDefaultHostResolver().GetHostAddressCount(
            m_endpoint, AWS_GET_HOST_ADDRESS_COUNT_RECORD_TYPE_A);

        EmitS3AddressCountMetric(numAddresses);
    }

    m_addressCache.clear();

    while ((uint32_t)m_addressCache.size() < desiredNumberOfAddresses)
    {
        std::mutex adressRetrievedMutex;
        std::condition_variable signal;
        bool resolveHostFinished = false;

        m_canaryApp.GetDefaultHostResolver().ResolveHost(
            m_endpoint,
            [this, &adressRetrievedMutex, &signal, &resolveHostFinished](
                Io::HostResolver &, const Vector<Io::HostAddress> &addresses, int) {
                for (const Io::HostAddress &addr : addresses)
                {
                    if (addr.record_type == AWS_ADDRESS_RECORD_TYPE_AAAA)
                    {
                        continue;
                    }

                    m_addressCache.emplace_back((const char *)addr.address->bytes);
                }

                {
                    std::lock_guard<std::mutex> lock(adressRetrievedMutex);
                    resolveHostFinished = true;
                }

                signal.notify_one();
            });

        std::unique_lock<std::mutex> waitLock(adressRetrievedMutex);
        signal.wait(waitLock, [&resolveHostFinished]() { return resolveHostFinished; });
    }

    AWS_LOGF_INFO(AWS_LS_CRT_CPP_CANARY, "DNS cache warmed.");
}

const String &S3ObjectTransport::GetAddressForTransfer(uint32_t index)
{
    index = (index / TransfersPerAddress) % (uint32_t)m_addressCache.size();
    return m_addressCache[index];
}

std::shared_ptr<Aws::Crt::Http::HttpClientConnectionManager> S3ObjectTransport::GetNextConnManager()
{
    if (m_connManagers.size() == 0)
    {
        AWS_LOGF_WARN(
            AWS_LS_CRT_CPP_CANARY,
            "No connection managers currently available.  Warming DNS cache and spawning connection managers");
        WarmDNSCache(1);
        SpawnConnectionManagers();
    }

    uint32_t index = ((m_connManagersUseCount.fetch_add(1) + 1) / TransfersPerAddress) % m_connManagers.size();
    return m_connManagers[index];
}

void S3ObjectTransport::SeedAddressCache(const String &address)
{
    m_addressCache.clear();

    m_addressCache.push_back(address);
}

void S3ObjectTransport::PurgeConnectionManagers()
{
    m_connManagers.clear();
    m_connManagersUseCount = 0;
}

void S3ObjectTransport::SpawnConnectionManagers()
{
    PurgeConnectionManagers();

    for (const String &address : m_addressCache)
    {
        AWS_LOGF_INFO(AWS_LS_CRT_CPP_CANARY, "Spawning connection manager for address %s", address.c_str());

        Http::HttpClientConnectionManagerOptions connectionManagerOptions;

        connectionManagerOptions.ConnectionOptions.HostName = address;
        connectionManagerOptions.ConnectionOptions.Port = m_canaryApp.GetOptions().sendEncrypted ? 443 : 80;
        connectionManagerOptions.ConnectionOptions.SocketOptions.SetConnectTimeoutMs(3000);
        connectionManagerOptions.ConnectionOptions.SocketOptions.SetSocketType(AWS_SOCKET_STREAM);
        connectionManagerOptions.ConnectionOptions.InitialWindowSize = SIZE_MAX;

        if (m_canaryApp.GetOptions().sendEncrypted)
        {
            aws_byte_cursor serverName = ByteCursorFromCString(m_endpoint.c_str());
            auto connOptions = m_canaryApp.GetTlsContext().NewConnectionOptions();
            connOptions.SetServerName(serverName);
            connectionManagerOptions.ConnectionOptions.TlsOptions = connOptions;
        }

        connectionManagerOptions.ConnectionOptions.Bootstrap = &m_canaryApp.GetBootstrap();
        connectionManagerOptions.MaxConnections = 5000;

        std::shared_ptr<Http::HttpClientConnectionManager> connManager =
            Http::HttpClientConnectionManager::NewClientConnectionManager(connectionManagerOptions, g_allocator);

        m_connManagers.push_back(connManager);
    }
}

void S3ObjectTransport::MakeSignedRequest(
    const std::shared_ptr<Http::HttpRequest> &request,
    const Http::HttpRequestOptions &requestOptions,
    SignedRequestCallback callback)
{
    String region = m_canaryApp.GetOptions().region.c_str();

    Auth::AwsSigningConfig signingConfig(g_allocator);
    signingConfig.SetRegion(region);
    signingConfig.SetCredentialsProvider(m_canaryApp.GetCredsProvider());
    signingConfig.SetService("s3");
    signingConfig.SetBodySigningType(Auth::BodySigningType::UnsignedPayload);
    signingConfig.SetSigningTimepoint(DateTime::Now());
    signingConfig.SetSigningAlgorithm(Auth::SigningAlgorithm::SigV4Header);

    m_canaryApp.GetSigner()->SignRequest(
        request,
        signingConfig,
        [this, requestOptions, callback](
            const std::shared_ptr<Aws::Crt::Http::HttpRequest> &signedRequest, int signingErrorCode) {
            if (signingErrorCode != AWS_ERROR_SUCCESS)
            {
                if (callback != nullptr)
                {
                    callback(nullptr, signingErrorCode);
                }
                return;
            }

            std::shared_ptr<Aws::Crt::Http::HttpClientConnectionManager> connManager = GetNextConnManager();

            connManager->AcquireConnection([this, requestOptions, signedRequest, callback](
                                               std::shared_ptr<Http::HttpClientConnection> conn, int connErrorCode) {
                if ((conn == nullptr || !conn->IsOpen()) && connErrorCode == AWS_ERROR_SUCCESS)
                {
                    connErrorCode = AWS_ERROR_UNKNOWN;
                }

                if (connErrorCode == AWS_ERROR_SUCCESS)
                {
                    MakeSignedRequest_SendRequest(conn, requestOptions, signedRequest);
                }

                if (callback != nullptr)
                {
                    callback(conn, connErrorCode);
                }
            });
        });
}

void S3ObjectTransport::MakeSignedRequest_SendRequest(
    const std::shared_ptr<Http::HttpClientConnection> &conn,
    const Http::HttpRequestOptions &requestOptions,
    const std::shared_ptr<Http::HttpRequest> &signedRequest)
{
    AWS_FATAL_ASSERT(conn->IsOpen());

    Http::HttpRequestOptions requestOptionsToSend = requestOptions;
    requestOptionsToSend.request = signedRequest.get();

    ++m_activeRequestsCount;

    // NOTE: The captures of the connection and signed request is a work around to keep those shared
    // pointers alive until the stream is finished.  Tasks can be scheduled that rely on these things
    // being alive which can cause crashes when they aren't around.
    requestOptionsToSend.onStreamComplete =
        [this, conn, signedRequest, requestOptions](Http::HttpStream &stream, int errorCode) {
            --m_activeRequestsCount;

            if (requestOptions.onStreamComplete != nullptr)
            {
                requestOptions.onStreamComplete(stream, errorCode);
            }
        };

    std::shared_ptr<Http::HttpClientStream> clientStream = conn->NewClientStream(requestOptionsToSend);

    if (clientStream == nullptr)
    {
        AWS_LOGF_ERROR(AWS_LS_CRT_CPP_CANARY, "Unable to open stream for S3ObjectTransport operation.");
    }
    else
    {
        clientStream->Activate();
    }
}

void S3ObjectTransport::AddContentLengthHeader(
    const std::shared_ptr<Http::HttpRequest> &request,
    const std::shared_ptr<Aws::Crt::Io::InputStream> &body)
{
    Http::HttpHeader contentLength;
    contentLength.name = ByteCursorFromCString("content-length");

    StringStream intValue;
    intValue << body->GetLength();
    String contentLengthVal = intValue.str();
    contentLength.value = ByteCursorFromCString(contentLengthVal.c_str());
    request->AddHeader(contentLength);
}

void S3ObjectTransport::PutObject(
    const Aws::Crt::String &key,
    const std::shared_ptr<Io::InputStream> &body,
    uint32_t flags,
    const PutObjectFinished &finishedCallback)
{
    AWS_FATAL_ASSERT(body.get() != nullptr);

    auto request = MakeShared<Http::HttpRequest>(g_allocator, g_allocator);

    AddContentLengthHeader(request, body);

    request->AddHeader(m_hostHeader);
    request->AddHeader(m_contentTypeHeader);
    request->SetBody(body);
    request->SetMethod(aws_http_method_put);

    StringStream keyPathStream;
    keyPathStream << "/" << key;

    String keyPath = keyPathStream.str();
    ByteCursor path = ByteCursorFromCString(keyPath.c_str());
    request->SetPath(path);

    AWS_LOGF_INFO(AWS_LS_CRT_CPP_CANARY, "PutObject initiated for path %s...", keyPath.c_str());

    std::shared_ptr<String> etag = nullptr;

    if ((flags & (uint32_t)EPutObjectFlags::RetrieveETag) != 0)
    {
        etag = MakeShared<String>(g_allocator);
    }

    Http::HttpRequestOptions requestOptions;
    AWS_ZERO_STRUCT(requestOptions);
    requestOptions.onIncomingHeaders = [etag](
                                           Http::HttpStream &stream,
                                           enum aws_http_header_block headerBlock,
                                           const Http::HttpHeader *headersArray,
                                           std::size_t headersCount) {
        (void)stream;
        (void)headerBlock;

        if (etag == nullptr)
        {
            return;
        }

        for (size_t i = 0; i < headersCount; ++i)
        {
            const aws_byte_cursor &name = headersArray[i].name;

            if (aws_byte_cursor_eq_c_str(&name, "ETag"))
            {
                const aws_byte_cursor &value = headersArray[i].value;
                *etag = String((const char *)value.ptr, value.len);
            }
        }
    };
    requestOptions.onStreamComplete = [keyPath, etag, finishedCallback](Http::HttpStream &stream, int errorCode) {
        if (errorCode == AWS_ERROR_SUCCESS)
        {
            if (stream.GetResponseStatusCode() != 200)
            {
                errorCode = AWS_ERROR_UNKNOWN;
            }

            aws_log_level logLevel = (errorCode != AWS_ERROR_SUCCESS) ? AWS_LL_ERROR : AWS_LL_INFO;

            AWS_LOGF(
                logLevel,
                AWS_LS_CRT_CPP_CANARY,
                "PutObject finished for path %s with response status %d",
                keyPath.c_str(),
                stream.GetResponseStatusCode());
        }
        else
        {
            AWS_LOGF_DEBUG(
                AWS_LS_CRT_CPP_CANARY,
                "PutObject finished for path %s with error '%s'",
                keyPath.c_str(),
                aws_error_debug_str(errorCode));
        }

        finishedCallback(errorCode, etag);
    };

    MakeSignedRequest(
        request,
        requestOptions,
        [keyPath, finishedCallback](std::shared_ptr<Http::HttpClientConnection> conn, int32_t errorCode) {
            if (errorCode != AWS_ERROR_SUCCESS)
            {
                AWS_LOGF_ERROR(AWS_LS_CRT_CPP_CANARY, "Making signed request failed with error code %d", errorCode);
                finishedCallback(errorCode, nullptr);
            }
        });
}

void S3ObjectTransport::GetObject(
    const Aws::Crt::String &key,
    uint32_t partNumber,
    Aws::Crt::Http::OnIncomingBody onIncomingBody,
    const GetObjectFinished &getObjectFinished)
{
    auto request = MakeShared<Http::HttpRequest>(g_allocator, g_allocator);
    request->AddHeader(m_hostHeader);

    request->SetMethod(aws_http_method_get);

    StringStream keyPathStream;
    keyPathStream << "/" << key;

    if (partNumber > 0)
    {
        keyPathStream << "?partNumber=" << partNumber;
    }

    String keyPath = keyPathStream.str();
    ByteCursor path = ByteCursorFromCString(keyPath.c_str());
    request->SetPath(path);

    Http::HttpRequestOptions requestOptions;
    AWS_ZERO_STRUCT(requestOptions);
    requestOptions.onIncomingBody = onIncomingBody;
    requestOptions.onStreamComplete = [keyPath, partNumber, getObjectFinished](Http::HttpStream &stream, int error) {
        int errorCode = error;

        if (errorCode == AWS_ERROR_SUCCESS)
        {
            int32_t successStatus = partNumber > 0 ? S3GetObjectResponseStatus_PartialContent : 200;

            if (stream.GetResponseStatusCode() != successStatus)
            {
                errorCode = AWS_ERROR_UNKNOWN;
            }

            aws_log_level logLevel = (errorCode != AWS_ERROR_SUCCESS) ? AWS_LL_ERROR : AWS_LL_DEBUG;

            AWS_LOGF(
                logLevel,
                AWS_LS_CRT_CPP_CANARY,
                "GetObject finished for path %s with response status %d",
                keyPath.c_str(),
                stream.GetResponseStatusCode());
        }
        else
        {
            AWS_LOGF_ERROR(
                AWS_LS_CRT_CPP_CANARY,
                "GetObject finished for path %s with error '%s'",
                keyPath.c_str(),
                aws_error_debug_str(errorCode));
        }

        getObjectFinished(errorCode);
    };

    MakeSignedRequest(
        request,
        requestOptions,
        [getObjectFinished](std::shared_ptr<Http::HttpClientConnection> conn, int32_t errorCode) {
            if (errorCode != AWS_ERROR_SUCCESS)
            {
                getObjectFinished(errorCode);
            }
        });
}

void S3ObjectTransport::PutObjectMultipart(
    const Aws::Crt::String &key,
    std::uint64_t objectSize,
    std::uint32_t numParts,
    SendPartCallback sendPart,
    const PutObjectMultipartFinished &finishedCallback)
{
    std::shared_ptr<MultipartUploadState> uploadState =
        MakeShared<MultipartUploadState>(g_allocator, key, objectSize, numParts);

    AWS_LOGF_INFO(
        AWS_LS_CRT_CPP_CANARY,
        "Put object multipart %s with object size %" PRId64 " and %d parts",
        key.c_str(),
        objectSize,
        numParts);

    // Set the callback that the MultipartTransferProcessor will use to process the part.
    // In this case, it will try to upload it.
    uploadState->SetProcessPartCallback([this, uploadState, sendPart, finishedCallback](
                                            const std::shared_ptr<TransferState> &transferState,
                                            MultipartTransferState::PartFinishedCallback partFinished) {
        std::shared_ptr<Io::InputStream> partInputStream = sendPart(transferState);

        UploadPart(uploadState, transferState, partInputStream, partFinished);
    });

    // Set the callback that will be called when something flags the upload state as finished.  Finished
    // can happen due to success or failure.
    uploadState->SetFinishedCallback([this, uploadState, key, numParts, finishedCallback](int32_t errorCode) {
        if (errorCode != AWS_ERROR_SUCCESS)
        {
            AbortMultipartUpload(key, uploadState->GetUploadId(), [numParts, errorCode, finishedCallback](int32_t) {
                finishedCallback(errorCode, numParts);
            });
        }
        else
        {
            finishedCallback(errorCode, numParts);
        }
    });

    // Go ahead and start the multipart upload, pushing it into the queue
    // if we were able to successfully issue the CreateMultipartUpload request.
    CreateMultipartUpload(uploadState->GetKey(), [this, uploadState](int errorCode, const Aws::Crt::String &uploadId) {
        if (errorCode != AWS_ERROR_SUCCESS)
        {
            uploadState->SetFinished(errorCode);
            return;
        }

        uploadState->SetUploadId(uploadId);
        m_uploadProcessor.PushQueue(uploadState);
    });
}

void S3ObjectTransport::GetObjectMultipart(
    const Aws::Crt::String &key,
    std::uint32_t numParts,
    const ReceivePartCallback &receivePart,
    const GetObjectMultipartFinished &finishedCallback)
{
    std::shared_ptr<MultipartDownloadState> downloadState =
        MakeShared<MultipartDownloadState>(g_allocator, key, 0L, numParts);

    // Set the callback that the MultipartTransferProcessor will use to process the part.
    // In this case, try to download it.
    downloadState->SetProcessPartCallback([this, downloadState, receivePart](
                                              const std::shared_ptr<TransferState> &transferState,
                                              const MultipartTransferState::PartFinishedCallback &partFinished) {
        GetPart(downloadState, transferState, receivePart, partFinished);
    });

    // Set the callback that will be called when something flags the upload state as finished.  Finished
    // can happen due to success or failure.
    downloadState->SetFinishedCallback([finishedCallback](int32_t errorCode) { finishedCallback(errorCode); });

    m_downloadProcessor.PushQueue(downloadState);
}

void S3ObjectTransport::UploadPart(
    const std::shared_ptr<MultipartUploadState> &state,
    const std::shared_ptr<TransferState> &transferState,
    const std::shared_ptr<Io::InputStream> &partInputStream,
    const MultipartTransferState::PartFinishedCallback &partFinished)
{
    StringStream keyPathStream;
    keyPathStream << state->GetKey() << "?partNumber=" << transferState->GetPartNumber()
                  << "&uploadId=" << state->GetUploadId();

    String keyPathStr = keyPathStream.str();

    transferState->AddDataUpMetric(0LL);

    PutObject(
        keyPathStr,
        partInputStream,
        (uint32_t)EPutObjectFlags::RetrieveETag,
        [this, state, transferState, partFinished, keyPathStr](
            int32_t errorCode, std::shared_ptr<Aws::Crt::String> etag) {
            if (etag == nullptr)
            {
                errorCode = AWS_ERROR_UNKNOWN;
            }

            if (errorCode == AWS_ERROR_SUCCESS)
            {
                state->SetETag(transferState->GetPartIndex(), *etag);

                if (state->IncNumPartsCompleted())
                {
                    Aws::Crt::Vector<Aws::Crt::String> etags;
                    state->GetETags(etags);

                    CompleteMultipartUpload(state->GetKey(), state->GetUploadId(), etags, [state](int32_t errorCode) {
                        state->SetFinished(errorCode);
                    });
                }

                partFinished(PartFinishResponse::Done);

                m_canaryApp.GetMetricsPublisher()->AddTransferStatusDataPoint(true);

                transferState->FlushDataUpMetrics();

                AWS_LOGF_INFO(
                    AWS_LS_CRT_CPP_CANARY,
                    "UploadPart for path %s and part #%d (%d/%d) just returned code %d",
                    state->GetKey().c_str(),
                    transferState->GetPartNumber(),
                    state->GetNumPartsCompleted(),
                    state->GetNumParts(),
                    errorCode);
            }
            else
            {
                AWS_LOGF_ERROR(
                    AWS_LS_CRT_CPP_CANARY,
                    "Upload part #%d failed with error code %d (\"%s\")",
                    transferState->GetPartNumber(),
                    errorCode,
                    aws_error_debug_str(errorCode));

                m_canaryApp.GetMetricsPublisher()->AddTransferStatusDataPoint(false);

                transferState->FlushDataUpMetrics();

                partFinished(PartFinishResponse::Retry);
            }
        });
}

void S3ObjectTransport::GetPart(
    const std::shared_ptr<MultipartDownloadState> &downloadState,
    const std::shared_ptr<TransferState> &transferState,
    const ReceivePartCallback &receiveObjectPartData,
    const MultipartTransferState::PartFinishedCallback &partFinished)
{
    transferState->AddDataDownMetric(0LL);

    GetObject(
        downloadState->GetKey(),
        transferState->GetPartNumber(),
        [downloadState, transferState, receiveObjectPartData](Http::HttpStream &stream, const ByteCursor &data) {
            (void)stream;

            transferState->AddDataDownMetric(data.len);

            receiveObjectPartData(transferState, data);
        },
        [downloadState, transferState, partFinished](int32_t errorCode) {
            const String &key = downloadState->GetKey();

            if (errorCode != AWS_ERROR_SUCCESS)
            {
                AWS_LOGF_ERROR(
                    AWS_LS_CRT_CPP_CANARY,
                    "Did not receive part #%d for %s",
                    transferState->GetPartNumber(),
                    key.c_str());

                transferState->FlushDataDownMetrics();

                partFinished(PartFinishResponse::Retry);
            }
            else
            {
                AWS_LOGF_INFO(
                    AWS_LS_CRT_CPP_CANARY, "Received part #%d for %s", transferState->GetPartNumber(), key.c_str());

                if (downloadState->IncNumPartsCompleted())
                {
                    AWS_LOGF_DEBUG(AWS_LS_CRT_CPP_CANARY, "Finished trying to get all parts for %s", key.c_str());
                    downloadState->SetFinished();
                }

                partFinished(PartFinishResponse::Done);

                transferState->FlushDataDownMetrics();
            }
        });
}

void S3ObjectTransport::CreateMultipartUpload(
    const Aws::Crt::String &key,
    const CreateMultipartUploadFinished &finishedCallback)
{
    auto request = MakeShared<Http::HttpRequest>(g_allocator, g_allocator);
    request->AddHeader(m_hostHeader);
    request->AddHeader(m_contentTypeHeader);
    request->SetMethod(aws_http_method_post);

    String keyPath = "/" + key + "?uploads";
    ByteCursor path = ByteCursorFromCString(keyPath.c_str());
    request->SetPath(path);

    std::shared_ptr<String> uploadId = MakeShared<String>(g_allocator);

    Http::HttpRequestOptions requestOptions;
    AWS_ZERO_STRUCT(requestOptions);
    requestOptions.onIncomingBody = [uploadId, keyPath](Http::HttpStream &stream, const ByteCursor &data) {
        (void)stream;

        const String uploadIdOpenTag("<UploadId>");
        const String uploadIdCloseTag("</UploadId>");

        String dataString((const char *)data.ptr);
        size_t uploadIdOpenIndex = dataString.find(uploadIdOpenTag);

        if (uploadIdOpenIndex == String::npos)
        {
            AWS_LOGF_ERROR(
                AWS_LS_CRT_CPP_CANARY, "CreateMultipartUpload response does not have an UploadId opening tag.");
            return;
        }

        size_t uploadIdCloseIndex = dataString.find(uploadIdCloseTag, uploadIdOpenIndex);

        if (uploadIdCloseIndex == String::npos)
        {
            AWS_LOGF_ERROR(
                AWS_LS_CRT_CPP_CANARY, "CreateMultipartUpload response does not have an UploadId closing tag.");
            return;
        }

        size_t uploadIdStart = uploadIdOpenIndex + uploadIdOpenTag.length();
        size_t uploadIdLength = uploadIdCloseIndex - uploadIdStart;

        *uploadId = dataString.substr(uploadIdStart, uploadIdLength);
    };
    requestOptions.onStreamComplete = [uploadId, keyPath, finishedCallback](Http::HttpStream &stream, int errorCode) {
        if (errorCode == AWS_ERROR_SUCCESS && uploadId->empty())
        {
            errorCode = AWS_ERROR_UNKNOWN;
        }

        if (errorCode == AWS_ERROR_SUCCESS)
        {
            if (stream.GetResponseStatusCode() != 200)
            {
                errorCode = AWS_ERROR_UNKNOWN;
            }

            aws_log_level logLevel = (errorCode != AWS_ERROR_SUCCESS) ? AWS_LL_ERROR : AWS_LL_DEBUG;

            AWS_LOGF(
                logLevel,
                AWS_LS_CRT_CPP_CANARY,
                "Created multipart upload for path %s with response status %d.",
                keyPath.c_str(),
                stream.GetResponseStatusCode());
        }
        else
        {
            AWS_LOGF_ERROR(
                AWS_LS_CRT_CPP_CANARY,
                "Created multipart upload for path %s failed with error '%s'",
                keyPath.c_str(),
                aws_error_debug_str(errorCode));
        }

        finishedCallback(errorCode, *uploadId);
    };

    AWS_LOGF_DEBUG(AWS_LS_CRT_CPP_CANARY, "Creating multipart upload for %s...", keyPath.c_str());

    MakeSignedRequest(
        request,
        requestOptions,
        [finishedCallback](std::shared_ptr<Http::HttpClientConnection> conn, int32_t errorCode) {
            if (errorCode != AWS_ERROR_SUCCESS)
            {
                finishedCallback(errorCode, "");
            }
        });
}

void S3ObjectTransport::CompleteMultipartUpload(
    const Aws::Crt::String &key,
    const Aws::Crt::String &uploadId,
    const Aws::Crt::Vector<Aws::Crt::String> &etags,
    const CompleteMultipartUploadFinished &finishedCallback)
{
    AWS_LOGF_DEBUG(AWS_LS_CRT_CPP_CANARY, "Completing multipart upload for %s...", key.c_str());

    auto request = MakeShared<Http::HttpRequest>(g_allocator, g_allocator);
    request->AddHeader(m_hostHeader);
    request->SetMethod(aws_http_method_post);

    std::shared_ptr<StringStream> xmlContents = MakeShared<StringStream>(g_allocator);
    *xmlContents << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n";
    *xmlContents << "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n";

    for (size_t i = 0; i < etags.size(); ++i)
    {
        const Aws::Crt::String &etag = etags[i];
        size_t partNumber = i + 1;

        *xmlContents << "   <Part>\n";
        *xmlContents << "       <ETag>" << etag << "</ETag>\n";
        *xmlContents << "       <PartNumber>" << partNumber << "</PartNumber>\n";
        *xmlContents << "   </Part>\n";
    }

    *xmlContents << "</CompleteMultipartUpload>";

    std::shared_ptr<Io::StdIOStreamInputStream> body =
        MakeShared<Io::StdIOStreamInputStream>(g_allocator, xmlContents, g_allocator);
    request->SetBody(body);

    AddContentLengthHeader(request, body);

    StringStream keyPathStream;
    keyPathStream << "/" << key << "?uploadId=" << uploadId;
    String keyPath = keyPathStream.str();
    ByteCursor path = ByteCursorFromCString(keyPath.c_str());
    request->SetPath(path);

    Http::HttpRequestOptions requestOptions;
    AWS_ZERO_STRUCT(requestOptions);
    requestOptions.onStreamComplete = [keyPath, finishedCallback](Http::HttpStream &stream, int errorCode) {
        if (errorCode == AWS_ERROR_SUCCESS)
        {
            if (stream.GetResponseStatusCode() != 200)
            {
                errorCode = AWS_ERROR_UNKNOWN;
            }

            aws_log_level logLevel = (errorCode != AWS_ERROR_SUCCESS) ? AWS_LL_ERROR : AWS_LL_DEBUG;

            AWS_LOGF(
                logLevel,
                AWS_LS_CRT_CPP_CANARY,
                "Finished multipart upload for path %s with response status %d.",
                keyPath.c_str(),
                stream.GetResponseStatusCode());
        }
        else
        {
            AWS_LOGF_ERROR(
                AWS_LS_CRT_CPP_CANARY,
                "Finished multipart upload for path %s with error '%s'",
                keyPath.c_str(),
                aws_error_debug_str(errorCode));
        }

        finishedCallback(errorCode);
    };

    MakeSignedRequest(
        request,
        requestOptions,
        [finishedCallback](std::shared_ptr<Http::HttpClientConnection> conn, int32_t errorCode) {
            if (errorCode != AWS_ERROR_SUCCESS)
            {
                finishedCallback(errorCode);
            }
        });
}

void S3ObjectTransport::AbortMultipartUpload(
    const Aws::Crt::String &key,
    const Aws::Crt::String &uploadId,
    const AbortMultipartUploadFinished &finishedCallback)
{
    AWS_LOGF_DEBUG(AWS_LS_CRT_CPP_CANARY, "Aborting multipart upload for %s...", key.c_str());

    auto request = MakeShared<Http::HttpRequest>(g_allocator, g_allocator);
    request->AddHeader(m_hostHeader);
    request->SetMethod(aws_http_method_delete);

    String keyPath = "/" + key + "?uploadId=" + uploadId;
    ByteCursor keyPathByteCursor = ByteCursorFromCString(keyPath.c_str());
    request->SetPath(keyPathByteCursor);

    Http::HttpRequestOptions requestOptions;
    AWS_ZERO_STRUCT(requestOptions);
    requestOptions.onStreamComplete = [uploadId, keyPath, finishedCallback](Http::HttpStream &stream, int errorCode) {
        if (errorCode == AWS_ERROR_SUCCESS)
        {
            if (stream.GetResponseStatusCode() != 204)
            {
                errorCode = AWS_ERROR_UNKNOWN;
            }

            aws_log_level logLevel = (errorCode != AWS_ERROR_SUCCESS) ? AWS_LL_ERROR : AWS_LL_DEBUG;

            AWS_LOGF(
                logLevel,
                AWS_LS_CRT_CPP_CANARY,
                "Abort multipart upload for path %s finished with response status %d.",
                keyPath.c_str(),
                stream.GetResponseStatusCode());
        }
        else
        {
            AWS_LOGF_ERROR(
                AWS_LS_CRT_CPP_CANARY,
                "Abort multipart upload for path %s failed with error '%s'",
                keyPath.c_str(),
                aws_error_debug_str(errorCode));
        }

        finishedCallback(errorCode);
    };

    MakeSignedRequest(
        request,
        requestOptions,
        [finishedCallback](std::shared_ptr<Http::HttpClientConnection> conn, int32_t errorCode) {
            if (errorCode != AWS_ERROR_SUCCESS)
            {
                finishedCallback(errorCode);
            }
        });
}
