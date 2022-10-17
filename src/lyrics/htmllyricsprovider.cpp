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

#include "config.h"

#include <memory>

#include <QObject>
#include <QByteArray>
#include <QVariant>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>

#include "core/logging.h"
#include "core/networkaccessmanager.h"
#include "core/utilities.h"
#include "htmllyricsprovider.h"
#include "lyricsfetcher.h"

HtmlLyricsProvider::HtmlLyricsProvider(const QString &name, const bool enabled, const QUrl &url, const QString &start_tag, const QString &end_tag, NetworkAccessManager *network, QObject *parent) : LyricsProvider(name, enabled, false, network, parent), url_(url), start_tag_(start_tag), end_tag_(end_tag) {}

HtmlLyricsProvider::~HtmlLyricsProvider() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }

}

bool HtmlLyricsProvider::StartSearch(const QString &artist, const QString &album, const QString &title, const int id) {

  QUrl url(GetUrl(artist, album, title));
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  QNetworkReply *reply = network_->get(req);
  replies_ << reply;
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, id, url]() { HandleLyricsReply(reply, id, url); });

  qLog(Debug) << name_ << "Sending request for" << url;

  return true;

}

void HtmlLyricsProvider::CancelSearch(const int id) { Q_UNUSED(id); }

QUrl HtmlLyricsProvider::GetUrl(const QString &artist, const QString &album, const QString &title) {

  QUrl url(url_);
  QString path = url.path();
  path.replace("${artist}", artist)
      .replace("${album}", album)
      .replace("${title}", title)
      .replace("${track}", title);
  url.setPath(path);

  return url;

}

void HtmlLyricsProvider::HandleLyricsReply(QNetworkReply *reply, const int id, const QUrl &url) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  if (reply->error() != QNetworkReply::NoError) {
    Error(QString("%1 (%2)").arg(reply->errorString()).arg(reply->error()));
    emit SearchFinished(id, LyricsSearchResults());
    return;
  }

  if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
    Error(QString("Received HTTP code %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
    emit SearchFinished(id, LyricsSearchResults());
    return;
  }

  QByteArray data = reply->readAll();
  QString html = QString::fromUtf8(data);

  QString lyrics = ParseLyricsFromHTML(html, QRegularExpression(start_tag_), QRegularExpression(end_tag_), QRegularExpression(start_tag_), false);

  LyricsSearchResults results;
  if (!lyrics.isEmpty()) {
    LyricsSearchResult result;
    result.lyrics = lyrics;
    results.append(result);
  }

  emit SearchFinished(id, results);

}

void HtmlLyricsProvider::Error(const QString &error, const QVariant &debug) {

  qLog(Error) << name_ << error;
  if (debug.isValid()) qLog(Debug) << debug;

}
