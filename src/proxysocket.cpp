#include "standard.h"
#include "utils.h"
#include "proxysocket.h"
#include "logger.h"

using namespace std;

ProxySocket::ProxySocket(int _fd, Protocol _inProto) {
    fd = _fd;
    protocol = _inProto;
    logger(VERB1) << "Accepted connection";
    logger(VERB1) << "This connection is in " << (protocol==PLAIN?"PLAIN":"HTTP");
    snprintf(ss, MAXHOSTBUFFERSIZE, "HTTP/1.1 200 OK\r\nContent-Length");

    setNonBlocking(fd);
}

ProxySocket::ProxySocket(char *host, int port, Protocol _outProto) {
    protocol = _outProto;
    logger(VERB1) << "Making outgoing connection to " << host << ":" << port;
    logger(VERB1) << "This connection is in " << (protocol==PLAIN?"PLAIN":"HTTP");
    if (snprintf(ss, MAXHOSTBUFFERSIZE,
                "GET %s / HTTP/1.0\r\nHost: %s:%d\r\nContent-Length",
                 host, host, port) >= MAXHOSTBUFFERSIZE) {
        logger(ERROR) << "Host name too long";
        exit(0);
    };
    // Open a socket file descriptor
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0){
        logger(ERROR) << "Could not open outward socket";
        exit(0);
    }

    // Standard syntax
    server = gethostbyname(host);
    if (!server) {
        logger(ERROR) << "Cannot reach host";
        exit(0);
    }

    bzero((char *)&servAddr, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&servAddr.sin_addr.s_addr,
          server->h_length);
    servAddr.sin_port = htons(port);

    // Connect to the server's socket
    if (connect(fd, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0) {
        logger(ERROR) << "Was connecting to " << host << ":" << port;
        logger(ERROR) << "Cannot connect to remote server";
        exit(0);
    }

    setNonBlocking(fd);
}

int ProxySocket::write(vector<char> &buffer, int size, int& from) {
    int bytesWritten;
    int headerBytes;
    int i, retval, failures;
    bool connectionBroken = false;
    char tmp[200];

    if (protocol == PLAIN) {

        failures = 0;
        bytesWritten = 0;
        headerBytes = 0;
        while (bytesWritten < size && failures < 50000) {
            retval = send(fd, &buffer[from+bytesWritten], size-bytesWritten, 0);
            if (retval == 0) {
                connectionBroken = true;
                failures += 10000;
            } else {
                connectionBroken = false;
                failures = 0;
                bytesWritten += retval;
            }
        }

    } else if (protocol == HTTP) {

        failures = 0;
        bytesWritten = 0;
        headerBytes = snprintf(tmp, 196, "%s: %d\r\n\r\n", ss, size);

        // Write HTTP header
        while (bytesWritten < headerBytes && failures < 50000) {
            retval = send(fd, &tmp[bytesWritten], headerBytes-bytesWritten, 0);
            if (retval == 0) {
                connectionBroken = true;
                failures += 10000;
            } else {
                connectionBroken = false;
                failures = 0;
                bytesWritten += retval;
            }
        }

        logger(VERB2) << "Wrote " << headerBytes << " as HTTP headers";

        // Write the remaining bytes
        if (!connectionBroken || bytesWritten == 0) {
            bytesWritten = 0;
            while (bytesWritten < size && failures < 50000) {
                retval = send(fd, &buffer[from+bytesWritten], size-bytesWritten, 0);
                if (retval == 0) {
                    connectionBroken = true;
                    failures += 10000;
                } else {
                    connectionBroken = false;
                    failures = 0;
                    bytesWritten += retval;
                }
            }
        }
        logger(VERB2) << "Wrote " << bytesWritten << " to HTTP socket";
    }

    if (connectionBroken) {
        return -1;
    } else {
        // This does not include the HTTP headers
        return bytesWritten;
    }
}

int ProxySocket::read(vector<char> &buffer, int from, int& respFrom) {
    int messageStart = -1;
    int messageLength = 0;
    int bytesRead = 0;
    int i, retval, failures;
    bool connectionBroken = false;
    int contentLengthPosition = -1;

    if (protocol == PLAIN) {
        // Updating reference
        respFrom = from;

        failures = 0;
        bytesRead = 0;

        // Read till either no bytes read for long
        // or if many bytes have been read
        // or connection has been broken for long
        while (failures < 50000 && bytesRead < 500) {

            // Don't accept too much data
            // it has to be written via HTTP
            // Leave space in buffer for headers
            retval = recv(fd, &buffer[from+bytesRead],
                        BUFSIZE-from-2-bytesRead-400, 0);
            if (retval == 0) {
                connectionBroken = true;
                failures += 10000;
            } else if (retval > 0) {
                connectionBroken = false;
                failures = 0;
                bytesRead += retval;
                messageLength += retval;
            } else if (retval == -1) {
                connectionBroken = false;
                failures++;
            }
        }

    } else if (protocol == HTTP) {
        failures = 0;
        bytesRead = 0;

        // Read till either no bytes read for long
        // or if headers have been read
        // or connection has been broken for long
        while ((failures < 50000 || bytesRead > 0) &&
               messageStart == -1 &&
               (failures < 50000 || !connectionBroken)) {
            retval = recv(fd, &buffer[from+bytesRead],
                        BUFSIZE-from-2-bytesRead, 0);
            if (retval == 0) {
                connectionBroken = true;
                failures += 10000;
            } else if (retval > 0) {
                connectionBroken = false;
                failures = 0;

                for (i=from+3; i<from+bytesRead+retval; i++) {
                    if (buffer[i-3] == '\r' && buffer[i-2] == '\n' &&
                        buffer[i-1] == '\r' && buffer[i-0] == '\n') {
                        messageStart = from+i+1;
                    }
                }

                bytesRead += retval;
            } else if (retval == -1) {
                connectionBroken = false;
                failures++;
            }
        }

        // Receive content if connection intact
        if (!connectionBroken && bytesRead > 0) {
            failures = 0;
            bytesRead = bytesRead - (messageStart - from);;
            messageLength = 0;

            // Get Content Length
            for (i=messageStart-1; i>=from; i--) {
                if (!strncmp(&buffer[i], "Content-Length", 14)) {
                    break;
                }
            }

            if (i < from) {
                logger(VERB2) << "Did not find Content-Length";
                return -2;
            }

            for (; i<messageStart; i++) {
                if (buffer[i] == ':') {
                    break;
                }
            }

            if (i == messageStart) {
                logger(VERB2) << "Did not find colon";
                return -2;
            }

            for (; i<messageStart; i++) {
                if (buffer[i] !=' ' &&
                    buffer[i] >= '0' && buffer[i] <= '9') {
                    break;
                }
            }

            if (i == messageStart) {
                logger(VERB2) << "Did not find numerical content length";
                return -2;
            }

            for (; i<messageStart; i++) {
                if (buffer[i] < '0' || buffer[i] > '9') {
                    break;
                } else {
                    messageLength *= 10;
                    messageLength += buffer[i] - '0';
                }
            }

            logger(VERB2) << "Content-Length: " << messageLength;

            logger(VERB2) << "Already read " << bytesRead << " out of " << messageLength;
            failures = 0;

            // Read till all bytes have been read
            // or connection has been broken for long
            while (failures < 50000 && bytesRead < messageLength) {
                retval = recv(fd, &buffer[messageStart+bytesRead],
                              messageLength-bytesRead, 0);
                if (retval == 0) {
                    connectionBroken = true;
                    failures += 10000;
                } else if (retval > 0) {
                    connectionBroken = false;
                    failures = 0;
                    bytesRead += retval;
                    logger(VERB2) << "Have read " << bytesRead << " now";
                }
            }

            respFrom = messageStart;
        }
    }

    if (connectionBroken) {
        return -1;
    } else {
        return messageLength;
    }
}

int ProxySocket::recvFromSocket(vector<char> &buffer, int from,
                                int &respFrom) {

    contentBytes = 0;
    receivedBytes = 0;
    numberOfFailures = 0;
    connectionBroken = false;
    gotHttpHeaders = -1;
    if (protocol == PLAIN) {
        logger(DEBUG) << "Recv PLAIN";
        do {
            retval = recv(fd, &buffer[receivedBytes+from],
                          BUFSIZE-from-2-receivedBytes, 0);
            if (retval < 0) {
                numberOfFailures++;
            } else {
                if (retval == 0) {
                    connectionBroken = true;
                    logger(VERB1) << "PLAIN connection broken";
                    numberOfFailures += 10000;
                } else {
                    connectionBroken = false;
                    numberOfFailures = 0;
                    receivedBytes += retval;
                }
            }
            // Now loop till either we don't receive anything for
            // 50000 bytes, or we've got 500 bytes of data
            // Both these are to ensure decent latency
        } while (numberOfFailures < 50000 && receivedBytes < 500);

        respFrom = 0;

        logger(DEBUG) << "Received " << receivedBytes << " bytes as plain.";

        if (connectionBroken == true && receivedBytes == 0) {
            logger(VERB1) << "Exiting because of broken Plain connection";
            return -1;
        } else if (connectionBroken == true) {
            logger(VERB1) << "PLAIN connection broken but will try again";
        }
        contentBytes = receivedBytes;

    } else if (protocol == HTTP) {
        logger(DEBUG) << "Recv HTTP";
        numberOfFailures = 0;
        do {
            retval = recv(fd, &buffer[receivedBytes+from],
                          BUFSIZE-from-2-receivedBytes, 0);
            if (retval == -1) {
                numberOfFailures++;
            } else if (retval == 0) {
                connectionBroken = true;
                logger(VERB1) << "HTTP connection broken";
                numberOfFailures += 10000;
            } else {
                connectionBroken = false;
                numberOfFailures = 0;
                receivedBytes += retval;
                for (k=from; k<receivedBytes+from-3; k++) {
                    if (buffer[k] == '\r' && buffer[k+1] == '\n' &&
                        buffer[k+2] == '\r' && buffer[k+3] == '\n') {
                        k += 4;
                        gotHttpHeaders = k-from;
                        break;
                    }
                }
            }
        } while (gotHttpHeaders == -1 && numberOfFailures < 50000);

        if (connectionBroken == true && receivedBytes == 0) {
            logger(VERB1) << "Exiting because of broken HTTP connection";
            return -1;
        } else if (receivedBytes == 0) {
            return 0;
        } else if (connectionBroken == true) {
            logger(VERB1) << "HTTP connection broken but will try again";
        }
        // If it was a normal HTTP response,
        // we should parse it
        logger(DEBUG) << "Received " << receivedBytes << " bytes as HTTP headers";
        logger(DEBUG) << &buffer[from];

        // Find content length header
        // TODO Make this more optimum
        for (k=from; k<receivedBytes+from; k++) {
            if (strncmp(&buffer[k], "Content-Length: ", 16) == 0) {
                break;
            }
        }

        // If we couldn't find the header
        if (k == receivedBytes+from) {
            logger(DEBUG) << "Didn't find content-length in headers";
            return 0;
        }

        // Point @k to the start of the content length int
        k += 17;
        int tp=0;
        for (int i = k; i<k+10; i++) {
            printf("%d: %d\n", i, buffer[i]);
        }
        printf("Start of content is: %d\n", gotHttpHeaders);

        while (buffer[k] >= '0' && buffer[k] <= '9') {
            tp *= 10;
            tp += buffer[k]-'0';
            k++;
        }

        contentLength = tp; // just rename tp to contentLength

        // for clarity, this variable should be the
        // number of bytes into the buffer where the header end and
        // content starts.
        startOfContent = gotHttpHeaders;

        respFrom = startOfContent; // Update this reference int

        logger(WARN) << "Looking for " << contentLength << "bytes";
        logger(WARN) << "Reading from " << (int)buffer[startOfContent-1] << "," << (int)buffer[startOfContent];

        // How many bytes we've read of the content
        contentBytes = receivedBytes - startOfContent;
        logger(WARN) << "ReceivedBytes: " << receivedBytes;
        bufferBytesRemaining = BUFSIZE-from-receivedBytes-2;

        while(contentBytes < contentLength && bufferBytesRemaining > 0) {

            // from is where the caller said start writing
            // contentBytes is how many bytes we've read making
            // up part of the content.
            // receivedBytes is the total number of bytes we've read
            // (marking the end of our buffer's valid content)
            // bufferBytesRemaining is how many bytes we have
            // remaining in the receive buffer

            retval = recv(fd, &buffer[from+startOfContent+contentBytes],
                          bufferBytesRemaining, 0);

            if (retval == 0) {
                logger(VERB1) << "HTTP Connection broken when receiving bytes";
                connectionBroken = true;
                break;
            } else if (retval > 0) {
                receivedBytes += retval;
                contentBytes += retval;
                bufferBytesRemaining -= retval;
            }
        }
        logger(DEBUG) << "Last byte: " << (int)buffer[from+startOfContent+contentBytes-1];
    }

    // Signal error, or return length of message
    if (connectionBroken == true && contentBytes == 0) {
        logger(VERB1) << "Exiting because of broken HTTP connection when receiving content";
        return -1;
    } else if (connectionBroken == true) {
        logger(VERB1) << "HTTP connection broken but will try again";
        return 0;
    } else {
        logger(VERB2) << "Received " << contentBytes << " as HTTP, sending to other end";
        return contentBytes;
    }
}

int ProxySocket::sendFromSocket(vector<char> &buffer, int from, int len) {

    sentBytes = 0;
    numberOfFailures = 0;
    if (protocol == PLAIN) {
        logger(DEBUG) << "Write PLAIN";
        sentBytes = send(fd, &buffer[from], len, 0);
    } else if (protocol == HTTP) {
        logger(DEBUG) << "Write HTTP";
        writtenBytes = snprintf(headers, MAXHOSTBUFFERSIZE+4,
                                "%s: %d\r\n\r\n", ss, len);
        if (writtenBytes >= MAXHOSTBUFFERSIZE+4) {
            logger(ERROR) << "Host name or content too long";
            exit(0);
        }
        logger(DEBUG) << "Wrote " << headers;
        sentBytes = send(fd, headers, writtenBytes, 0);
        if (sentBytes < 1) {
            return -1;
        }
        sentBytes = send(fd, &buffer[from], len, 0);
        buffer[from+len] = 0;
        logger(DEBUG) << "Wrote now " << &buffer[from];
    }
    return sentBytes;
}

void ProxySocket::sendHelloMessage() {
    sentBytes = 0;
    writtenBytes = 0;
    do {
        writtenBytes = send(fd, "GET / HTTP/1.1\r\n\r\n", 18, 0);
        if (writtenBytes == 0) {
            logger(ERROR) << "Connection closed at handshake";
            exit(0);
        } else if (writtenBytes > 0) {
            sentBytes += writtenBytes;
        }
    } while (sentBytes < 18);
}

void ProxySocket::receiveHelloMessage() {
    receivedBytes = 0;

    char tmp[20];
    do {
        readBytes = recv(fd, tmp, 18, 0);
        if (readBytes == 0) {
            logger(ERROR) << "Connection closed at handshake";
            exit(0);
        } else if (readBytes > 0) {
            receivedBytes += readBytes;
        }
    } while (receivedBytes < 18);
}

void ProxySocket::closeSocket() {
    close(fd);
}
