/*
 * Strawberry Music Player
 * Copyright 2022, Jonas Kvinge <jonas@jkvinge.net>
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

#ifndef HTMLLYRICSPROVIDER_H
#define HTMLLYRICSPROVIDER_H

#include "config.h"

#include <memory>

#include <QtGlobal>
#include <QObject>
#include <QList>
#include <QVariant>
#include <QString>
#include <QUrl>

#include "lyricsprovider.h"
#include "lyricsfetcher.h"

class QNetworkReply;
class NetworkAccessManager;

class HtmlLyricsProvider : public LyricsProvider {
  Q_OBJECT

 public:
  explicit HtmlLyricsProvider(const QString &name, const bool enabled, const QUrl &url, const QString &start_tag, const QString &end_tag, NetworkAccessManager *network, QObject *parent);
  ~HtmlLyricsProvider();

  bool StartSearch(const QString &artist, const QString &album, const QString &title, const int id) override;
  void CancelSearch(const int id) override;

 private:
  QUrl GetUrl(const QString &artist, const QString &album, const QString &title);
  void Error(const QString &error, const QVariant &debug = QVariant()) override;

 private slots:
  void HandleLyricsReply(QNetworkReply *reply, const int id, const QUrl &url);

 protected:
  QList<QNetworkReply*> replies_;
  QUrl url_;
  QString start_tag_;
  QString end_tag_;

};

#endif  // HTMLLYRICSPROVIDER_H
