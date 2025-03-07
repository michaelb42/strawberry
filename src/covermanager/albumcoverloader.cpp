/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2019-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <memory>

#include <QtGlobal>
#include <QObject>
#include <QStandardPaths>
#include <QDir>
#include <QThread>
#include <QMutex>
#include <QBuffer>
#include <QSet>
#include <QList>
#include <QQueue>
#include <QVariant>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "core/networkaccessmanager.h"
#include "core/song.h"
#include "core/tagreaderclient.h"
#include "utilities/mimeutils.h"
#include "utilities/imageutils.h"
#include "albumcoverloader.h"
#include "albumcoverloaderoptions.h"
#include "albumcoverloaderresult.h"
#include "albumcoverimageresult.h"

AlbumCoverLoader::AlbumCoverLoader(QObject *parent)
    : QObject(parent),
      network_(new NetworkAccessManager(this)),
      stop_requested_(false),
      load_image_async_id_(1),
      save_image_async_id_(1),
      original_thread_(nullptr) {

  original_thread_ = thread();

}

void AlbumCoverLoader::ExitAsync() {

  stop_requested_ = true;
  QMetaObject::invokeMethod(this, "Exit", Qt::QueuedConnection);

}

void AlbumCoverLoader::Exit() {

  Q_ASSERT(QThread::currentThread() == thread());
  moveToThread(original_thread_);
  emit ExitFinished();

}

void AlbumCoverLoader::CancelTask(const quint64 id) {

  QMutexLocker l(&mutex_load_image_async_);
  for (QQueue<TaskPtr>::iterator it = tasks_.begin(); it != tasks_.end(); ++it) {
    TaskPtr task = *it;
    if (task->id == id) {
      tasks_.erase(it);  // clazy:exclude=strict-iterators
      break;
    }
  }

}

void AlbumCoverLoader::CancelTasks(const QSet<quint64> &ids) {

  QMutexLocker l(&mutex_load_image_async_);
  for (QQueue<TaskPtr>::iterator it = tasks_.begin(); it != tasks_.end();) {
    TaskPtr task = *it;
    if (ids.contains(task->id)) {
      it = tasks_.erase(it);  // clazy:exclude=strict-iterators
    }
    else {
      ++it;
    }
  }

}

quint64 AlbumCoverLoader::LoadImageAsync(const AlbumCoverLoaderOptions &options, const Song &song) {

  TaskPtr task = std::make_shared<Task>();
  task->options = options;
  task->song = song;
  task->state = State::Manual;
  task->album_cover = std::make_shared<AlbumCoverImageResult>();

  return EnqueueTask(task);

}

quint64 AlbumCoverLoader::LoadImageAsync(const AlbumCoverLoaderOptions &options, const QUrl &art_automatic, const QUrl &art_manual, const QUrl &song_url, const Song::Source song_source) {

  Song song(song_source);
  song.set_url(song_url);
  song.set_art_automatic(art_automatic);
  song.set_art_manual(art_manual);

  TaskPtr task = std::make_shared<Task>();
  task->options = options;
  task->song = song;
  task->state = State::Manual;
  task->album_cover = std::make_shared<AlbumCoverImageResult>();

  return EnqueueTask(task);

}

quint64 AlbumCoverLoader::LoadImageAsync(const AlbumCoverLoaderOptions &options, AlbumCoverImageResultPtr album_cover) {

  TaskPtr task = std::make_shared<Task>();
  task->options = options;
  task->album_cover = album_cover;

  return EnqueueTask(task);

}

quint64 AlbumCoverLoader::LoadImageAsync(const AlbumCoverLoaderOptions &options, const QImage &image) {

  TaskPtr task = std::make_shared<Task>();
  task->options = options;
  task->album_cover = std::make_shared<AlbumCoverImageResult>();
  task->album_cover->image = image;

  return EnqueueTask(task);

}

quint64 AlbumCoverLoader::EnqueueTask(TaskPtr task) {

  {
    QMutexLocker l(&mutex_load_image_async_);
    task->id = load_image_async_id_++;
    tasks_.enqueue(task);
  }

  QMetaObject::invokeMethod(this, "ProcessTasks", Qt::QueuedConnection);

  return task->id;

}

void AlbumCoverLoader::ProcessTasks() {

  while (!stop_requested_) {
    // Get the next task
    TaskPtr task;
    {
      QMutexLocker l(&mutex_load_image_async_);
      if (tasks_.isEmpty()) return;
      task = tasks_.dequeue();
    }

    ProcessTask(task);
  }

}

void AlbumCoverLoader::ProcessTask(TaskPtr task) {

  TryLoadResult result = TryLoadImage(task);
  if (result.started_async) {
    // The image is being loaded from a remote URL, we'll carry on later when it's done
    return;
  }

  if (result.loaded_success) {
    result.album_cover->mime_type = Utilities::MimeTypeFromData(result.album_cover->image_data);
    QImage image_scaled;
    QImage image_thumbnail;
    if (task->options.get_image_ && task->options.scale_output_image_) {
      image_scaled = ImageUtils::ScaleAndPad(result.album_cover->image, task->options.scale_output_image_, task->options.pad_output_image_, task->options.desired_height_);
    }
    if (task->options.get_image_ && task->options.create_thumbnail_) {
      image_thumbnail = ImageUtils::CreateThumbnail(result.album_cover->image, task->options.pad_thumbnail_image_, task->options.thumbnail_size_);
    }
    emit AlbumCoverLoaded(task->id, std::make_shared<AlbumCoverLoaderResult>(result.loaded_success, result.type, result.album_cover, image_scaled, image_thumbnail, task->art_updated));
    return;
  }

  NextState(task);

}

void AlbumCoverLoader::NextState(TaskPtr task) {

  if (task->state == State::Manual) {
    // Try the automatic one next
    task->state = State::Automatic;
    ProcessTask(task);
  }
  else {
    // Give up
    emit AlbumCoverLoaded(task->id, std::make_shared<AlbumCoverLoaderResult>(false, AlbumCoverLoaderResult::Type::None, std::make_shared<AlbumCoverImageResult>(task->options.default_output_image_), task->options.default_scaled_image_, task->options.default_thumbnail_image_, task->art_updated));
  }

}

AlbumCoverLoader::TryLoadResult AlbumCoverLoader::TryLoadImage(TaskPtr task) {

  // Only scale and pad.
  if (task->album_cover->is_valid()) {
    return TryLoadResult(false, true, AlbumCoverLoaderResult::Type::Embedded, task->album_cover);
  }

  // For local files and streams initialize art if found.
  if ((task->song.source() == Song::Source::LocalFile || task->song.is_radio()) && !task->song.art_manual_is_valid() && !task->song.art_automatic_is_valid()) {
    switch (task->state) {
      case State::None:
        break;
      case State::Manual:
        task->song.InitArtManual();
        if (task->song.art_manual_is_valid()) task->art_updated = true;
        break;
      case State::Automatic:
        if (task->song.url().isLocalFile()) {
          task->song.InitArtAutomatic();
          if (task->song.art_automatic_is_valid()) task->art_updated = true;
        }
        break;
    }
  }

  AlbumCoverLoaderResult::Type type = AlbumCoverLoaderResult::Type::None;
  QUrl cover_url;
  switch (task->state) {
    case State::None:
    case State::Automatic:
      type = AlbumCoverLoaderResult::Type::Automatic;
      cover_url = task->song.art_automatic();
      break;
    case State::Manual:
      type = AlbumCoverLoaderResult::Type::Manual;
      cover_url = task->song.art_manual();
      break;
  }
  task->type = type;

  if (!cover_url.isEmpty() && !cover_url.path().isEmpty()) {
    if (cover_url.path() == Song::kManuallyUnsetCover) {
      return TryLoadResult(false, true, AlbumCoverLoaderResult::Type::ManuallyUnset, std::make_shared<AlbumCoverImageResult>(cover_url, QString(), QByteArray(), task->options.default_output_image_));
    }
    else if (cover_url.path() == Song::kEmbeddedCover && task->song.url().isLocalFile()) {
      QByteArray image_data = TagReaderClient::Instance()->LoadEmbeddedArtBlocking(task->song.url().toLocalFile());
      if (!image_data.isEmpty()) {
        QImage image;
        if (!image_data.isEmpty() && task->options.get_image_ && image.loadFromData(image_data)) {
          return TryLoadResult(false, !image.isNull(), AlbumCoverLoaderResult::Type::Embedded, std::make_shared<AlbumCoverImageResult>(cover_url, QString(), image_data, image));
        }
        else {
          return TryLoadResult(false, !image_data.isEmpty(), AlbumCoverLoaderResult::Type::Embedded, std::make_shared<AlbumCoverImageResult>(cover_url, QString(), image_data, image));
        }
      }
    }

    if (cover_url.isLocalFile()) {
      QFile file(cover_url.toLocalFile());
      if (file.exists()) {
        if (file.open(QIODevice::ReadOnly)) {
          QByteArray image_data = file.readAll();
          file.close();
          QImage image;
          if (!image_data.isEmpty() && task->options.get_image_ && image.loadFromData(image_data)) {
            return TryLoadResult(false, !image.isNull(), type, std::make_shared<AlbumCoverImageResult>(cover_url, QString(), image_data, image.isNull() ? task->options.default_output_image_ : image));
          }
          else {
            return TryLoadResult(false, !image_data.isEmpty(), type, std::make_shared<AlbumCoverImageResult>(cover_url, QString(), image_data, image.isNull() ? task->options.default_output_image_ : image));
          }
        }
        else {
          qLog(Error) << "Failed to open cover file" << cover_url << "for reading" << file.errorString();
        }
      }
      else {
        qLog(Error) << "Cover file" << cover_url << "does not exist";
      }
    }
    else if (cover_url.scheme().isEmpty()) {  // Assume a local file with no scheme.
      QFile file(cover_url.path());
      if (file.exists()) {
        if (file.open(QIODevice::ReadOnly)) {
          QByteArray image_data = file.readAll();
          file.close();
          QImage image;
          if (!image_data.isEmpty() && task->options.get_image_ && image.loadFromData(image_data)) {
            return TryLoadResult(false, !image.isNull(), type, std::make_shared<AlbumCoverImageResult>(cover_url, QString(), image_data, image.isNull() ? task->options.default_output_image_ : image));
          }
          else {
            return TryLoadResult(false, !image_data.isEmpty(), type, std::make_shared<AlbumCoverImageResult>(cover_url, QString(), image_data, image.isNull() ? task->options.default_output_image_ : image));
          }
        }
        else {
          qLog(Error) << "Failed to open cover file" << cover_url << "for reading" << file.errorString();
        }
      }
      else {
        qLog(Error) << "Cover file" << cover_url << "does not exist";
      }
    }
    else if (network_->supportedSchemes().contains(cover_url.scheme())) {  // Remote URL
      qLog(Debug) << "Loading remote cover from" << cover_url;
      QNetworkRequest request(cover_url);
      request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
      QNetworkReply *reply = network_->get(request);
      QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, cover_url]() { RemoteFetchFinished(reply, cover_url); });

      remote_tasks_.insert(reply, task);
      return TryLoadResult(true, false, type, std::make_shared<AlbumCoverImageResult>(cover_url));
    }
  }

  return TryLoadResult(false, false, AlbumCoverLoaderResult::Type::None, std::make_shared<AlbumCoverImageResult>(cover_url, QString(), QByteArray(), task->options.default_output_image_));

}

void AlbumCoverLoader::RemoteFetchFinished(QNetworkReply *reply, const QUrl &cover_url) {

  reply->deleteLater();

  if (!remote_tasks_.contains(reply)) return;
  TaskPtr task = remote_tasks_.take(reply);

  // Handle redirects.
  QVariant redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
  if (redirect.isValid()) {
    if (++task->redirects > kMaxRedirects) {
      return;  // Give up.
    }
    QNetworkRequest request = reply->request();
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setUrl(redirect.toUrl());
    QNetworkReply *redirected_reply = network_->get(request);
    QObject::connect(redirected_reply, &QNetworkReply::finished, this, [this, redirected_reply, redirect]() { RemoteFetchFinished(redirected_reply, redirect.toUrl()); });

    remote_tasks_.insert(redirected_reply, task);
    return;
  }

  if (reply->error() == QNetworkReply::NoError) {
    // Try to load the image
    QByteArray image_data = reply->readAll();
    QString mime_type = Utilities::MimeTypeFromData(image_data);
    QImage image;
    if (image.loadFromData(image_data)) {
      QImage image_scaled;
      QImage image_thumbnail;
      if (task->options.scale_output_image_) image_scaled = ImageUtils::ScaleAndPad(image, task->options.scale_output_image_, task->options.pad_output_image_, task->options.desired_height_);
      if (task->options.create_thumbnail_) image_thumbnail = ImageUtils::CreateThumbnail(image, task->options.pad_thumbnail_image_, task->options.thumbnail_size_);
      emit AlbumCoverLoaded(task->id, std::make_shared<AlbumCoverLoaderResult>(true, task->type, std::make_shared<AlbumCoverImageResult>(cover_url, mime_type, (task->options.get_image_data_ ? image_data : QByteArray()), image), image_scaled, image_thumbnail, task->art_updated));
      return;
    }
    else {
      qLog(Error) << "Unable to load album cover image" << cover_url;
    }
  }
  else {
    qLog(Error) << "Unable to get album cover" << cover_url << reply->error() << reply->errorString();
  }

  NextState(task);

}

quint64 AlbumCoverLoader::SaveEmbeddedCoverAsync(const QString &song_filename, const QString &cover_filename) {

  QMutexLocker l(&mutex_save_image_async_);
  quint64 id = ++save_image_async_id_;
  QMetaObject::invokeMethod(this, "SaveEmbeddedCover", Qt::QueuedConnection, Q_ARG(quint64, id), Q_ARG(QString, song_filename), Q_ARG(QString, cover_filename));
  return id;

}

quint64 AlbumCoverLoader::SaveEmbeddedCoverAsync(const QString &song_filename, const QImage &image) {

  QMutexLocker l(&mutex_save_image_async_);
  quint64 id = ++save_image_async_id_;
  QMetaObject::invokeMethod(this, "SaveEmbeddedCover", Qt::QueuedConnection, Q_ARG(quint64, id), Q_ARG(QString, song_filename), Q_ARG(QImage, image));
  return id;

}

quint64 AlbumCoverLoader::SaveEmbeddedCoverAsync(const QString &song_filename, const QByteArray &image_data) {

  QMutexLocker l(&mutex_save_image_async_);
  quint64 id = ++save_image_async_id_;
  QMetaObject::invokeMethod(this, "SaveEmbeddedCover", Qt::QueuedConnection, Q_ARG(quint64, id), Q_ARG(QString, song_filename), Q_ARG(QByteArray, image_data));
  return id;

}

quint64 AlbumCoverLoader::SaveEmbeddedCoverAsync(const QList<QUrl> &urls, const QString &cover_filename) {

  QMutexLocker l(&mutex_save_image_async_);
  quint64 id = ++save_image_async_id_;
  QMetaObject::invokeMethod(this, "SaveEmbeddedCover", Qt::QueuedConnection, Q_ARG(quint64, id), Q_ARG(QList<QUrl>, urls), Q_ARG(QString, cover_filename));
  return id;

}

quint64 AlbumCoverLoader::SaveEmbeddedCoverAsync(const QList<QUrl> &urls, const QImage &image) {

  QMutexLocker l(&mutex_save_image_async_);
  quint64 id = ++save_image_async_id_;
  QMetaObject::invokeMethod(this, "SaveEmbeddedCover", Qt::QueuedConnection, Q_ARG(quint64, id), Q_ARG(QList<QUrl>, urls), Q_ARG(QImage, image));
  return id;

}

quint64 AlbumCoverLoader::SaveEmbeddedCoverAsync(const QList<QUrl> &urls, const QByteArray &image_data) {

  QMutexLocker l(&mutex_save_image_async_);
  quint64 id = ++save_image_async_id_;
  QMetaObject::invokeMethod(this, "SaveEmbeddedCover", Qt::QueuedConnection, Q_ARG(quint64, id), Q_ARG(QList<QUrl>, urls), Q_ARG(QByteArray, image_data));
  return id;

}

void AlbumCoverLoader::SaveEmbeddedCover(const quint64 id, const QString &song_filename, const QByteArray &image_data) {

  TagReaderReply *reply = TagReaderClient::Instance()->SaveEmbeddedArt(song_filename, TagReaderClient::SaveCoverOptions(image_data));
  tagreader_save_embedded_art_requests_.insert(id, reply);
  const bool clear = image_data.isEmpty();
  QObject::connect(reply, &TagReaderReply::Finished, this, [this, id, reply, clear]() { SaveEmbeddedArtFinished(id, reply, clear); }, Qt::QueuedConnection);

}

void AlbumCoverLoader::SaveEmbeddedCover(const quint64 id, const QString &song_filename, const QImage &image) {

  QByteArray image_data;

  if (!image.isNull()) {
    QBuffer buffer(&image_data);
    if (buffer.open(QIODevice::WriteOnly)) {
      image.save(&buffer, "JPEG");
      buffer.close();
    }
  }

  SaveEmbeddedCover(id, song_filename, image_data);

}

void AlbumCoverLoader::SaveEmbeddedCover(const quint64 id, const QString &song_filename, const QString &cover_filename) {

  QFile file(cover_filename);

  if (file.size() >= 209715200) {  // Max 200 MB.
    emit SaveEmbeddedCoverAsyncFinished(id, false, false);
    return;
  }

  if (!file.open(QIODevice::ReadOnly)) {
    qLog(Error) << "Failed to open cover file" << cover_filename << "for reading:" << file.errorString();
    emit SaveEmbeddedCoverAsyncFinished(id, false, false);
    return;
  }

  QByteArray image_data = file.readAll();
  file.close();

  SaveEmbeddedCover(id, song_filename, image_data);

}

void AlbumCoverLoader::SaveEmbeddedCover(const quint64 id, const QList<QUrl> &urls, const QImage &image) {

  if (image.isNull()) {
    for (const QUrl &url : urls) {
      SaveEmbeddedCover(id, url.toLocalFile(), QByteArray());
    }
    return;
  }
  else {
    QByteArray image_data;
    QBuffer buffer(&image_data);
    if (buffer.open(QIODevice::WriteOnly)) {
      if (image.save(&buffer, "JPEG")) {
        SaveEmbeddedCover(id, urls, image_data);
        buffer.close();
        return;
      }
      buffer.close();
    }
  }

  emit SaveEmbeddedCoverAsyncFinished(id, false, image.isNull());

}

void AlbumCoverLoader::SaveEmbeddedCover(const quint64 id, const QList<QUrl> &urls, const QString &cover_filename) {

  QFile file(cover_filename);

  if (file.size() >= 209715200) {  // Max 200 MB.
    emit SaveEmbeddedCoverAsyncFinished(id, false, false);
    return;
  }

  if (!file.open(QIODevice::ReadOnly)) {
    qLog(Error) << "Failed to open cover file" << cover_filename << "for reading:" << file.errorString();
    emit SaveEmbeddedCoverAsyncFinished(id, false, false);
    return;
  }

  QByteArray image_data = file.readAll();
  file.close();
  SaveEmbeddedCover(id, urls, image_data);

}

void AlbumCoverLoader::SaveEmbeddedCover(const quint64 id, const QList<QUrl> &urls, const QByteArray &image_data) {

  for (const QUrl &url : urls) {
    SaveEmbeddedCover(id, url.toLocalFile(), image_data);
  }

}

void AlbumCoverLoader::SaveEmbeddedArtFinished(const quint64 id, TagReaderReply *reply, const bool cleared) {

  if (tagreader_save_embedded_art_requests_.contains(id)) {
    tagreader_save_embedded_art_requests_.remove(id, reply);
  }

  if (!tagreader_save_embedded_art_requests_.contains(id)) {
    emit SaveEmbeddedCoverAsyncFinished(id, reply->is_successful(), cleared);
  }

  reply->deleteLater();

}
