// The MIT License (MIT)
//
// Copyright (c) Itay Grudev 2015 - 2020
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

//
//  W A R N I N G !!!
//  -----------------
//
// This is a modified version of SingleApplication,
// The original version is at:
//
// https://github.com/itay-grudev/SingleApplication
//
//

#include "config.h"

#include <QtGlobal>

#include <cstdlib>
#include <cstddef>

#ifdef Q_OS_UNIX
#  include <unistd.h>
#  include <sys/types.h>
#  include <pwd.h>
#endif

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
#  include <windows.h>
#  include <lmcons.h>
#endif

#include <QObject>
#include <QThread>
#include <QIODevice>
#include <QSharedMemory>
#include <QByteArray>
#include <QDataStream>
#include <QCryptographicHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QRandomGenerator>

#include "singlecoreapplication.h"
#include "singlecoreapplication_p.h"

SingleCoreApplicationPrivate::SingleCoreApplicationPrivate(SingleCoreApplication *ptr)
    : q_ptr(ptr),
      memory_(nullptr),
      socket_(nullptr),
      server_(nullptr),
      instanceNumber_(-1) {}

SingleCoreApplicationPrivate::~SingleCoreApplicationPrivate() {

  if (socket_ != nullptr) {
    socket_->close();
    delete socket_;
    socket_ = nullptr;
  }

  if (memory_ != nullptr) {
    memory_->lock();
    InstancesInfo *instance = static_cast<InstancesInfo*>(memory_->data());
    if (server_ != nullptr) {
      server_->close();
      delete server_;
      instance->primary = false;
      instance->primaryPid = -1;
      instance->primaryUser[0] = '\0';
      instance->checksum = blockChecksum();
    }
    memory_->unlock();

    delete memory_;
    memory_ = nullptr;
  }

}

QString SingleCoreApplicationPrivate::getUsername() {

#ifdef Q_OS_UNIX
  QString username;
#if defined(HAVE_GETEUID) && defined(HAVE_GETPWUID)
  struct passwd *pw = getpwuid(geteuid());
  if (pw) {
    username = QString::fromLocal8Bit(pw->pw_name);
  }
#endif
  if (username.isEmpty()) {
    username = qEnvironmentVariable("USER");
  }
  return username;
#endif

#ifdef Q_OS_WIN
  wchar_t username[UNLEN + 1];
  // Specifies size of the buffer on input
  DWORD usernameLength = UNLEN + 1;
  if (GetUserNameW(username, &usernameLength)) {
    return QString::fromWCharArray(username);
  }
  return qEnvironmentVariable("USERNAME");
#endif

}

void SingleCoreApplicationPrivate::genBlockServerName() {

  QCryptographicHash appData(QCryptographicHash::Sha256);
  appData.addData("SingleApplication");
  appData.addData(SingleCoreApplication::app_t::applicationName().toUtf8());
  appData.addData(SingleCoreApplication::app_t::organizationName().toUtf8());
  appData.addData(SingleCoreApplication::app_t::organizationDomain().toUtf8());

  if (!(options_ & SingleCoreApplication::Mode::ExcludeAppVersion)) {
    appData.addData(SingleCoreApplication::app_t::applicationVersion().toUtf8());
  }

  if (!(options_ & SingleCoreApplication::Mode::ExcludeAppPath)) {
#if defined(Q_OS_UNIX)
    const QByteArray appImagePath = qgetenv("APPIMAGE");
    if (appImagePath.isEmpty()) {
      appData.addData(SingleCoreApplication::app_t::applicationFilePath().toUtf8());
    }
    else {
      appData.addData(appImagePath);
    };
#elif defined(Q_OS_WIN)
    appData.addData(SingleCoreApplication::app_t::applicationFilePath().toLower().toUtf8());
#else
    appData.addData(SingleCoreApplication::app_t::applicationFilePath().toUtf8());
#endif
  }

  // User level block requires a user specific data in the hash
  if (options_ & SingleCoreApplication::Mode::User) {
    appData.addData(getUsername().toUtf8());
  }

  // Replace the backslash in RFC 2045 Base64 [a-zA-Z0-9+/=] to comply with server naming requirements.
  blockServerName_ = appData.result().toBase64().replace("/", "_");

}

void SingleCoreApplicationPrivate::initializeMemoryBlock() const {

  InstancesInfo *instance = static_cast<InstancesInfo*>(memory_->data());
  instance->primary = false;
  instance->secondary = 0;
  instance->primaryPid = -1;
  instance->primaryUser[0] = '\0';
  instance->checksum = blockChecksum();

}

void SingleCoreApplicationPrivate::startPrimary() {

  // Reset the number of connections
  InstancesInfo *instance = static_cast<InstancesInfo*>(memory_->data());

  instance->primary = true;
  instance->primaryPid = QCoreApplication::applicationPid();
  qstrncpy(instance->primaryUser, getUsername().toUtf8().data(), sizeof(instance->primaryUser));
  instance->checksum = blockChecksum();
  instanceNumber_ = 0;
  // Successful creation means that no main process exists
  // So we start a QLocalServer to listen for connections
  QLocalServer::removeServer(blockServerName_);
  server_ = new QLocalServer();

  // Restrict access to the socket according to the SingleCoreApplication::Mode::User flag on User level or no restrictions
  if (options_ & SingleCoreApplication::Mode::User) {
    server_->setSocketOptions(QLocalServer::UserAccessOption);
  }
  else {
    server_->setSocketOptions(QLocalServer::WorldAccessOption);
  }

  server_->listen(blockServerName_);
  QObject::connect(server_, &QLocalServer::newConnection, this, &SingleCoreApplicationPrivate::slotConnectionEstablished);

}

void SingleCoreApplicationPrivate::startSecondary() {

  InstancesInfo *instance = static_cast<InstancesInfo*>(memory_->data());

  instance->secondary += 1;
  instance->checksum = blockChecksum();
  instanceNumber_ = instance->secondary;

}

bool SingleCoreApplicationPrivate::connectToPrimary(const int timeout, const ConnectionType connectionType) {

  QElapsedTimer time;
  time.start();

  // Connect to the Local Server of the Primary Instance if not already connected.
  if (socket_ == nullptr) {
    socket_ = new QLocalSocket();
  }

  if (socket_->state() == QLocalSocket::ConnectedState) return true;

  if (socket_->state() != QLocalSocket::ConnectedState) {

    forever {
      randomSleep();

      if (socket_->state() != QLocalSocket::ConnectingState) {
        socket_->connectToServer(blockServerName_);
      }

      if (socket_->state() == QLocalSocket::ConnectingState) {
        socket_->waitForConnected(static_cast<int>(timeout - time.elapsed()));
      }

      // If connected break out of the loop
      if (socket_->state() == QLocalSocket::ConnectedState) break;

      // If elapsed time since start is longer than the method timeout return
      if (time.elapsed() >= timeout) return false;
    }
  }

  // Initialisation message according to the SingleCoreApplication protocol
  QByteArray initMsg;
  QDataStream writeStream(&initMsg, QIODevice::WriteOnly);
  writeStream.setVersion(QDataStream::Qt_5_8);

  writeStream << blockServerName_.toLatin1();
  writeStream << static_cast<quint8>(connectionType);
  writeStream << instanceNumber_;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  quint16 checksum = qChecksum(QByteArray(initMsg, static_cast<quint32>(initMsg.length())));
#else
  quint16 checksum = qChecksum(initMsg.constData(), static_cast<quint32>(initMsg.length()));
#endif

  writeStream << checksum;

  return writeConfirmedMessage(static_cast<int>(timeout - time.elapsed()), initMsg);

}

void SingleCoreApplicationPrivate::writeAck(QLocalSocket *sock) {
  sock->putChar('\n');
}

bool SingleCoreApplicationPrivate::writeConfirmedMessage(const int timeout, const QByteArray &msg) const {

  QElapsedTimer time;
  time.start();

  // Frame 1: The header indicates the message length that follows
  QByteArray header;
  QDataStream headerStream(&header, QIODevice::WriteOnly);
  headerStream.setVersion(QDataStream::Qt_5_8);
  headerStream << static_cast<quint64>(msg.length());

  if (!writeConfirmedFrame(static_cast<int>(timeout - time.elapsed()), header)) {
    return false;
  }

  // Frame 2: The message
  return writeConfirmedFrame(static_cast<int>(timeout - time.elapsed()), msg);

}

bool SingleCoreApplicationPrivate::writeConfirmedFrame(const int timeout, const QByteArray &msg) const {

  socket_->write(msg);
  socket_->flush();

  bool result = socket_->waitForReadyRead(timeout);
  if (result) {
    socket_->read(1);
    return true;
  }

  return false;

}

quint16 SingleCoreApplicationPrivate::blockChecksum() const {

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  quint16 checksum = qChecksum(QByteArray(static_cast<const char*>(memory_->constData()), offsetof(InstancesInfo, checksum)));
#else
  quint16 checksum = qChecksum(static_cast<const char*>(memory_->constData()), offsetof(InstancesInfo, checksum));
#endif

  return checksum;

}

qint64 SingleCoreApplicationPrivate::primaryPid() const {

  memory_->lock();
  InstancesInfo *instance = static_cast<InstancesInfo*>(memory_->data());
  qint64 pid = instance->primaryPid;
  memory_->unlock();

  return pid;

}

QString SingleCoreApplicationPrivate::primaryUser() const {

  memory_->lock();
  InstancesInfo *instance = static_cast<InstancesInfo*>(memory_->data());
  QByteArray username = instance->primaryUser;
  memory_->unlock();

  return QString::fromUtf8(username);

}

/**
 * @brief Executed when a connection has been made to the LocalServer
 */
void SingleCoreApplicationPrivate::slotConnectionEstablished() {

  QLocalSocket *nextConnSocket = server_->nextPendingConnection();
  connectionMap_.insert(nextConnSocket, ConnectionInfo());

  QObject::connect(nextConnSocket, &QLocalSocket::aboutToClose, this, [nextConnSocket, this]() {
    const ConnectionInfo &info = connectionMap_[nextConnSocket];
    slotClientConnectionClosed(nextConnSocket, info.instanceId);
  });

  QObject::connect(nextConnSocket, &QLocalSocket::disconnected, nextConnSocket, &QLocalSocket::deleteLater);

  QObject::connect(nextConnSocket, &QLocalSocket::destroyed, this, [nextConnSocket, this]() {
    connectionMap_.remove(nextConnSocket);
  });

  QObject::connect(nextConnSocket, &QLocalSocket::readyRead, this, [nextConnSocket, this]() {
    const ConnectionInfo &info = connectionMap_[nextConnSocket];
    switch (info.stage) {
      case StageInitHeader:
        readMessageHeader(nextConnSocket, StageInitBody);
        break;
      case StageInitBody:
        readInitMessageBody(nextConnSocket);
        break;
      case StageConnectedHeader:
        readMessageHeader(nextConnSocket, StageConnectedBody);
        break;
      case StageConnectedBody:
        this->slotDataAvailable(nextConnSocket, info.instanceId);
        break;
      default:
        break;
    };
  });

}

void SingleCoreApplicationPrivate::readMessageHeader(QLocalSocket *sock, SingleCoreApplicationPrivate::ConnectionStage nextStage) {

  if (!connectionMap_.contains(sock)) {
    return;
  }

  if (sock->bytesAvailable() < static_cast<qint64>(sizeof(quint64))) {
    return;
  }

  QDataStream headerStream(sock);
  headerStream.setVersion(QDataStream::Qt_5_8);

  // Read the header to know the message length
  quint64 msgLen = 0;
  headerStream >> msgLen;
  ConnectionInfo &info = connectionMap_[sock];
  info.stage = nextStage;
  info.msgLen = msgLen;

  writeAck(sock);

}

bool SingleCoreApplicationPrivate::isFrameComplete(QLocalSocket *sock) {

  if (!connectionMap_.contains(sock)) {
    return false;
  }

  ConnectionInfo &info = connectionMap_[sock];
  return (sock->bytesAvailable() >= static_cast<qint64>(info.msgLen));

}

void SingleCoreApplicationPrivate::readInitMessageBody(QLocalSocket *sock) {

  Q_Q(SingleCoreApplication);

  if (!isFrameComplete(sock)) {
    return;
  }

  // Read the message body
  QByteArray msgBytes = sock->readAll();
  QDataStream readStream(msgBytes);
  readStream.setVersion(QDataStream::Qt_5_8);

  // server name
  QByteArray latin1Name;
  readStream >> latin1Name;

  // connection type
  ConnectionType connectionType = InvalidConnection;
  quint8 connTypeVal = InvalidConnection;
  readStream >> connTypeVal;
  connectionType = static_cast<ConnectionType>(connTypeVal);

  // instance id
  quint32 instanceId = 0;
  readStream >> instanceId;

  // checksum
  quint16 msgChecksum = 0;
  readStream >> msgChecksum;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  const quint16 actualChecksum = qChecksum(QByteArray(msgBytes, static_cast<quint32>(msgBytes.length() - sizeof(quint16))));
#else
  const quint16 actualChecksum = qChecksum(msgBytes.constData(), static_cast<quint32>(msgBytes.length() - sizeof(quint16)));
#endif

  bool isValid = readStream.status() == QDataStream::Ok && QLatin1String(latin1Name) == blockServerName_ && msgChecksum == actualChecksum;

  if (!isValid) {
    sock->close();
    return;
  }

  ConnectionInfo &info = connectionMap_[sock];
  info.instanceId = instanceId;
  info.stage = StageConnectedHeader;

  if (connectionType == NewInstance || (connectionType == SecondaryInstance && options_ & SingleCoreApplication::Mode::SecondaryNotification)) {
    emit q->instanceStarted();
  }

  writeAck(sock);

}

void SingleCoreApplicationPrivate::slotDataAvailable(QLocalSocket *dataSocket, const quint32 instanceId) {

  Q_Q(SingleCoreApplication);

  if (!isFrameComplete(dataSocket)) {
    return;
  }

  const QByteArray message = dataSocket->readAll();

  writeAck(dataSocket);

  ConnectionInfo &info = connectionMap_[dataSocket];
  info.stage = StageConnectedHeader;

  emit q->receivedMessage(instanceId, message);

}

void SingleCoreApplicationPrivate::slotClientConnectionClosed(QLocalSocket *closedSocket, const quint32 instanceId) {

  if (closedSocket->bytesAvailable() > 0) {
    slotDataAvailable(closedSocket, instanceId);
  }

}

void SingleCoreApplicationPrivate::randomSleep() {

  QThread::msleep(QRandomGenerator::global()->bounded(8U, 18U));

}
