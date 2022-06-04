/*
 * Strawberry Music Player
 * This file was part of Amarok / Clementine
 * Copyright 2003 Mark Kretschmann
 * Copyright 2004 - 2005 Max Howell, <max.howell@methylblue.com>
 * Copyright 2010 David Sansome <me@davidsansome.com>
 * Copyright 2017-2021 Jonas Kvinge <jonas@jkvinge.net>
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

#ifndef ENGINEBASE_H
#define ENGINEBASE_H

#include "config.h"

#include <sys/types.h>
#include <cstdint>
#include <vector>

#include <QtGlobal>
#include <QObject>
#include <QList>
#include <QMetaType>
#include <QVariant>
#include <QString>
#include <QUrl>

#include "engine_fwd.h"
#include "enginetype.h"
#include "devicefinders.h"
#include "core/song.h"

namespace Engine {

struct SimpleMetaBundle;

using Scope = std::vector<int16_t>;

class Base : public QObject {
  Q_OBJECT

 protected:
  Base(const EngineType type = EngineType::None, QObject *parent = nullptr);

 public:
  ~Base() override;

  struct OutputDetails {
    QString name;
    QString description;
    QString iconname;
  };
  using OutputDetailsList = QList<OutputDetails>;

  virtual bool Init() = 0;
  virtual State state() const = 0;
  virtual void StartPreloading(const QUrl&, const QUrl&, const bool, const qint64, const qint64) {}
  virtual bool Load(const QUrl &stream_url, const QUrl &original_url, const TrackChangeFlags change, const bool force_stop_at_end, const quint64 beginning_nanosec, const qint64 end_nanosec);
  virtual bool Play(const quint64 offset_nanosec) = 0;
  virtual void Stop(const bool stop_after = false) = 0;
  virtual void Pause() = 0;
  virtual void Unpause() = 0;
  virtual void Seek(const quint64 offset_nanosec) = 0;
  virtual void SetVolumeSW(const uint percent) = 0;

  virtual qint64 position_nanosec() const = 0;
  virtual qint64 length_nanosec() const = 0;

  virtual const Scope &scope(const int chunk_length) { Q_UNUSED(chunk_length); return scope_; }

  // Sets new values for the beginning and end markers of the currently playing song.
  // This doesn't change the state of engine or the stream's current position.
  virtual void RefreshMarkers(const quint64 beginning_nanosec, const qint64 end_nanosec) {
    beginning_nanosec_ = beginning_nanosec;
    end_nanosec_ = end_nanosec;
  }

  virtual OutputDetailsList GetOutputsList() const = 0;
  virtual bool ValidOutput(const QString &output) = 0;
  virtual QString DefaultOutput() = 0;
  virtual bool CustomDeviceSupport(const QString &output) = 0;
  virtual bool ALSADeviceSupport(const QString &output) = 0;

  // Plays a media stream represented with the URL 'u' from the given 'beginning' to the given 'end' (usually from 0 to a song's length).
  // Both markers should be passed in nanoseconds. 'end' can be negative, indicating that the real length of 'u' stream is unknown.
  bool Play(const QUrl &stream_url, const QUrl &original_url, const TrackChangeFlags flags, const bool force_stop_at_end, const quint64 beginning_nanosec, const qint64 end_nanosec, const quint64 offset_nanosec);
  void SetVolume(const uint volume);

 public slots:
  virtual void ReloadSettings();
  void UpdateVolume(const uint volume);

 protected:
  void EmitAboutToEnd();

 public:

  // Simple accessors
  EngineType type() const { return type_; }
  bool volume_control() const { return volume_control_; }
  inline uint volume() const { return volume_; }

  bool is_fadeout_enabled() const { return fadeout_enabled_; }
  bool is_crossfade_enabled() const { return crossfade_enabled_; }
  bool is_autocrossfade_enabled() const { return autocrossfade_enabled_; }
  bool crossfade_same_album() const { return crossfade_same_album_; }
  bool IsEqualizerEnabled() { return equalizer_enabled_; }

  static const int kScopeSize = 1024;

  QVariant device() { return device_; }

 public slots:
  virtual void SetStereoBalancerEnabled(const bool) {}
  virtual void SetStereoBalance(const float) {}
  virtual void SetEqualizerEnabled(const bool) {}
  virtual void SetEqualizerParameters(const int, const QList<int>&) {}

 signals:
  // Emitted when crossfading is enabled and the track is crossfade_duration_ away from finishing
  void TrackAboutToEnd();

  void TrackEnded();

  void FadeoutFinishedSignal();

  void StatusText(QString text);
  void Error(QString text);

  // Emitted when there was a fatal error
  void FatalError();
  // Emitted when Engine was unable to play a song with the given QUrl.
  void InvalidSongRequested(QUrl url);
  // Emitted when Engine successfully started playing a song with the given QUrl.
  void ValidSongRequested(QUrl url);

  void MetaData(Engine::SimpleMetaBundle);

  // Signals that the engine's state has changed (a stream was stopped for example).
  // Always use the state from event, because it's not guaranteed that immediate subsequent call to state() won't return a stale value.
  void StateChanged(Engine::State);

  void VolumeChanged(uint volume);

 protected:

  struct PluginDetails {
    QString name;
    QString description;
    QString iconname;
  };
  using PluginDetailsList = QList<PluginDetails>;

  EngineType type_;
  bool volume_control_;
  uint volume_;
  quint64 beginning_nanosec_;
  qint64 end_nanosec_;
  QUrl stream_url_;
  QUrl original_url_;
  Scope scope_;
  bool buffering_;
  bool equalizer_enabled_;

  // Settings
  QString output_;
  QVariant device_;

  // ReplayGain
  bool rg_enabled_;
  int rg_mode_;
  double rg_preamp_;
  double rg_fallbackgain_;
  bool rg_compression_;

  // Buffering
  quint64 buffer_duration_nanosec_;
  double buffer_low_watermark_;
  double buffer_high_watermark_;

  // Fadeout
  bool fadeout_enabled_;
  bool crossfade_enabled_;
  bool autocrossfade_enabled_;
  bool crossfade_same_album_;
  bool fadeout_pause_enabled_;
  qint64 fadeout_duration_;
  qint64 fadeout_duration_nanosec_;
  qint64 fadeout_pause_duration_;
  qint64 fadeout_pause_duration_nanosec_;

  // Proxy
  QString proxy_address_;
  bool proxy_authentication_;
  QString proxy_user_;
  QString proxy_pass_;

  // Channels
  bool channels_enabled_;
  int channels_;

  // Options
  bool bs2b_enabled_;
  bool http2_enabled_;

  // Spotify
#ifdef HAVE_SPOTIFY
  QString spotify_username_;
  QString spotify_password_;
#endif

 private:
  bool about_to_end_emitted_;
  Q_DISABLE_COPY(Base)

};

struct SimpleMetaBundle {
  SimpleMetaBundle() : type(Type::Any), length(-1), year(-1), track(-1), filetype(Song::FileType::Unknown), samplerate(-1), bitdepth(-1), bitrate(-1) {}
  enum class Type {
    Any,
    Current,
    Next
  };
  Type type;
  QUrl url;
  QUrl stream_url;
  QString title;
  QString artist;
  QString album;
  QString comment;
  QString genre;
  qint64 length;
  int year;
  int track;
  Song::FileType filetype;
  int samplerate;
  int bitdepth;
  int bitrate;
  QString lyrics;
};

}  // namespace Engine

Q_DECLARE_METATYPE(EngineBase::OutputDetails)
Q_DECLARE_METATYPE(Engine::SimpleMetaBundle)

#endif  // ENGINEBASE_H
