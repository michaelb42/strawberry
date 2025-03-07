/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2011, David Sansome <me@davidsansome.com>
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

#ifndef TAGREADERCLIENT_H
#define TAGREADERCLIENT_H

#include "config.h"

#include <QObject>
#include <QList>
#include <QString>
#include <QImage>

#include "core/messagehandler.h"
#include "core/workerpool.h"

#include "song.h"
#include "tagreadermessages.pb.h"

class QThread;
class Song;
template<typename HandlerType> class WorkerPool;

class TagReaderClient : public QObject {
  Q_OBJECT

 public:
  explicit TagReaderClient(QObject *parent = nullptr);

  using HandlerType = AbstractMessageHandler<spb::tagreader::Message>;
  using ReplyType = HandlerType::ReplyType;

  static const char *kWorkerExecutableName;

  void Start();
  void ExitAsync();

  enum class SaveTags {
    Off,
    On
  };

  enum class SavePlaycount {
    Off,
    On
  };

  enum class SaveRating {
    Off,
    On
  };

  class SaveCoverOptions {
   public:
    explicit SaveCoverOptions(const bool _enabled = false, const bool _is_jpeg = false, const QString &_cover_filename = QString(), const QByteArray &_cover_data = QByteArray()) : enabled(_enabled), is_jpeg(_is_jpeg), cover_filename(_cover_filename), cover_data(_cover_data) {}
    explicit SaveCoverOptions(const QString &_cover_filename) : enabled(true), is_jpeg(false), cover_filename(_cover_filename) {}
    explicit SaveCoverOptions(const QByteArray &_cover_data) : enabled(true), is_jpeg(false), cover_data(_cover_data) {}
    bool enabled;
    bool is_jpeg;
    QString cover_filename;
    QByteArray cover_data;
  };

  ReplyType *IsMediaFile(const QString &filename);
  ReplyType *ReadFile(const QString &filename);
  ReplyType *SaveFile(const QString &filename, const Song &metadata, const SaveTags save_tags = SaveTags::On, const SavePlaycount save_playcount = SavePlaycount::Off, const SaveRating save_rating = SaveRating::Off, const SaveCoverOptions &save_cover_options = SaveCoverOptions());
  ReplyType *LoadEmbeddedArt(const QString &filename);
  ReplyType *SaveEmbeddedArt(const QString &filename, const SaveCoverOptions &save_cover_options);
  ReplyType *UpdateSongPlaycount(const Song &metadata);
  ReplyType *UpdateSongRating(const Song &metadata);

  // Convenience functions that call the above functions and wait for a response.
  // These block the calling thread with a semaphore, and must NOT be called from the TagReaderClient's thread.
  void ReadFileBlocking(const QString &filename, Song *song);
  bool SaveFileBlocking(const QString &filename, const Song &metadata, const SaveTags save_tags = SaveTags::On, const SavePlaycount save_playcount = SavePlaycount::Off, const SaveRating save_rating = SaveRating::Off, const SaveCoverOptions &save_cover_options = SaveCoverOptions());
  bool IsMediaFileBlocking(const QString &filename);
  QByteArray LoadEmbeddedArtBlocking(const QString &filename);
  QImage LoadEmbeddedArtAsImageBlocking(const QString &filename);
  bool SaveEmbeddedArtBlocking(const QString &filename, const SaveCoverOptions &save_cover_options);
  bool UpdateSongPlaycountBlocking(const Song &metadata);
  bool UpdateSongRatingBlocking(const Song &metadata);

  // TODO: Make this not a singleton
  static TagReaderClient *Instance() { return sInstance; }

 signals:
  void ExitFinished();

 private slots:
  void Exit();
  void WorkerFailedToStart();

 public slots:
  void UpdateSongsPlaycount(const SongList &songs);
  void UpdateSongsRating(const SongList &songs);

 private:
  static TagReaderClient *sInstance;

  WorkerPool<HandlerType> *worker_pool_;
  QList<spb::tagreader::Message> message_queue_;
  QThread *original_thread_;
};

using TagReaderReply = TagReaderClient::ReplyType;

#endif  // TAGREADERCLIENT_H
