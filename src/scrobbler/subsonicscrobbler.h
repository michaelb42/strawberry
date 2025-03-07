/*
 * Strawberry Music Player
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 * Copyright 2020, Pascal Below <spezifisch@below.fr>
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

#ifndef SUBSONICSCROBBLER_H
#define SUBSONICSCROBBLER_H

#include "config.h"

#include <QtGlobal>
#include <QObject>
#include <QDateTime>
#include <QVariant>
#include <QString>
#include <QTimer>

#include "core/song.h"
#include "scrobblerservice.h"

class Application;
class AudioScrobbler;
class SubsonicService;

class SubsonicScrobbler : public ScrobblerService {
  Q_OBJECT

 public:
  explicit SubsonicScrobbler(AudioScrobbler *scrobbler, SubsonicService *service, QObject *parent = nullptr);

  static const char *kName;

  void ReloadSettings() override;

  bool IsEnabled() const override { return enabled_; }
  bool IsAuthenticated() const override { return true; }

  void UpdateNowPlaying(const Song &song) override;
  void ClearPlaying() override;
  void Scrobble(const Song &song) override;

  void StartSubmit(const bool initial = false) override { Q_UNUSED(initial) }
  void Submitted() override { submitted_ = true; }
  bool IsSubmitted() const override { return submitted_; }

 public slots:
  void WriteCache() override {}
  void Submit() override;

 private:
  AudioScrobbler *scrobbler_;
  SubsonicService *service_;
  bool enabled_;
  bool submitted_;
  Song song_playing_;
  QDateTime time_;
  QTimer timer_submit_;

};

#endif  // SUBSONICSCROBBLER_H
