/*
 * QMPDClient - An MPD client written in Qt 4.
 * Copyright (C) 2005-2008 Håvard Tautra Knutsen <havtknut@tihlde.org>
 * Copyright (C) 2009 Voker57 <voker57@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "lastfmsubmitter.h"
#include "config.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QStringList>
#include <QCryptographicHash>
#include <QTimer>

#include <QDebug>

LastFmSubmitter::LastFmSubmitter(QObject * parent) : QObject(parent) {
	m_npPending = false;
	m_awaitingHS = false;
	//m_awaitingScrob = false;
	m_failed = 0;
	m_session = "";
	m_hsUrl="http://post.audioscrobbler.com/";
	m_npUrl = ""; // we got it after handshake
	m_subUrl = ""; // we got it after handshake
	m_hardFailTimer = new QTimer(this);
	m_hardFailTimer->setInterval(10*1000);
	m_hardFailTimer->setSingleShot(true);
	m_scrobbleTimer = new QTimer(this);
	m_scrobbleTimer->setSingleShot(true);
	m_npTimer = new QTimer(this);
	m_npTimer->setSingleShot(true);
	m_npTimer->setInterval(5000);
	m_scrobbleRetryTimer = new QTimer(this);
	m_scrobbleRetryTimer->setSingleShot(true);
	m_scrobbleRetryTimer->setInterval(10000);
	m_netAccess = new QNetworkAccessManager(this);
	connect(m_netAccess, SIGNAL(finished(QNetworkReply *)), this, SLOT(gotNetReply(QNetworkReply *)));
	connect(m_scrobbleTimer, SIGNAL(timeout()), this, SLOT(scrobbleCurrent()));
	connect(m_scrobbleRetryTimer, SIGNAL(timeout()), this, SLOT(scrobbleQueued()));
	connect(m_npTimer, SIGNAL(timeout()), this, SLOT(sendNowPlaying()));
	connect(m_hardFailTimer, SIGNAL(timeout()), this, SLOT(doHandshake()));
}

void LastFmSubmitter::setSong(const MPDSong & s) {
	if(m_currentSong != s)
	{
		m_currentSong = s;
		m_currentStarted = time(NULL);
		if(s.type() == MPDSong::PLAYLISTSTREAM)
		{
			m_scrobbleTimer->setInterval(30*1000); // How else should i handle _stream_?
			// qDebug() << "starting scrobble timer" << m_scrobbleTimer->interval();
			m_scrobbleTimer->start();
		} else
		if(s.secs() > 30)
		{
			m_scrobbleTimer->setInterval((s.secs() < 480 ? s.secs()/10 : 10) * 1000);
			// qDebug() << "starting scrobble timer" << m_scrobbleTimer->interval();
			m_scrobbleTimer->start();
		}
		m_npTimer->start();
	}
}

void LastFmSubmitter::sendNowPlaying() {
	m_npPending = true;
	if (ensureHandshaked()) {
		scrobbleNp(m_currentSong);
		m_npPending = false;
	}
}

void LastFmSubmitter::scrobbleNp(MPDSong & s) {
	// kinda ugly code
	QString data = QString("s=%1&").arg(m_session);
	data += QString("a=%1&").arg(QString(QUrl::toPercentEncoding(s.artist())));
	data += QString("t=%1&").arg(QString(QUrl::toPercentEncoding(s.title())));
	data += QString("b=%1&").arg(QString(QUrl::toPercentEncoding(s.album())));
	data += QString("l=%1&").arg(s.secs() >0 ? QString::number(s.secs()) : "100");
	data += QString("n=%1").arg(QString(QUrl::toPercentEncoding(s.track())));
	// qDebug() << data;
	m_netAccess->post(QNetworkRequest(QUrl(m_npUrl)), data.toAscii());
}

void LastFmSubmitter::scrobbleCurrent() {
	m_songQueue.enqueue(QPair<MPDSong, int>(m_currentSong, m_currentStarted));
	//emit infoMsg(tr("Will scrobble this track."));
	scrobbleQueued();
}

void LastFmSubmitter::scrobbleQueued() {
	if (!ensureHandshaked()) {
		if (!m_scrobbleRetryTimer->isActive())
			m_scrobbleRetryTimer->start();
		return;
	}
	
	QString data = QString("s=%1&").arg(m_session);
	int i = 0;
	emit infoMsg(tr("Scrobbling %1 songs...").arg(m_songQueue.size()));
	qDebug() << "trying scrobble "<< m_songQueue.size() << " songs";
	while(!m_songQueue.isEmpty())
	{
		QPair<MPDSong, int> sPair = m_songQueue.dequeue();
		data += QString("r[%2]=&m[%2]=&a[%2]=%1&").arg(QString(QUrl::toPercentEncoding(sPair.first.artist())), QString::number(i));
		data += QString("o[%2]=%1&").arg(
			(
		(sPair.first.type() == MPDSong::STREAM) || (sPair.first.type() == MPDSong::PLAYLISTSTREAM)) ? "E" : "P",
			QString::number(i)
			);
		data += QString("t[%2]=%1&").arg(QString(QUrl::toPercentEncoding(sPair.first.title())), QString::number(i));
		data += QString("b[%2]=%1&").arg(QString(QUrl::toPercentEncoding(sPair.first.album())), QString::number(i));
		data += QString("l[%2]=%1&").arg(sPair.first.secs() >0 ? QString::number(sPair.first.secs()) : "", QString::number(i));
		data += QString("i[%2]=%1&").arg(QString::number(sPair.second), QString::number(i));
		data += QString("n[%2]=%1").arg(QString(QUrl::toPercentEncoding(sPair.first.track())), QString::number(i));
		++i;
	}
	if (i>0) {
		qDebug() << "sending scrobble to " << m_subUrl.toAscii();
		qDebug() << "data: " << data.toAscii();
		m_netAccess->post(QNetworkRequest(QUrl(m_subUrl)), data.toAscii());
	}
}

bool LastFmSubmitter::ensureHandshaked() {
	if(!m_session.isEmpty())
		return true;
	doHandshake();
	return false;
}

QByteArray LastFmSubmitter::getPasswordHash() {
	QByteArray passwordHash;
	if (Config::instance()->lastFmHashedPassword())
		passwordHash = Config::instance()->lastFmPassword().toAscii();
	else
		passwordHash = QCryptographicHash::hash(
			Config::instance()->lastFmPassword().toAscii(),
			QCryptographicHash::Md5).toHex();

	//accomplish it with current time
	passwordHash = QCryptographicHash::hash(
			passwordHash + QByteArray::number((uint)time(NULL)),
			QCryptographicHash::Md5);

	return passwordHash;
}

void LastFmSubmitter::doHandshake() {
	if (m_hardFailTimer->isActive() || m_awaitingHS) {
		qDebug("handshaking delayed");
		return;
	}
	QUrl hsUrl = QUrl(m_hsUrl);
	hsUrl.addQueryItem("hs", "true");
	hsUrl.addQueryItem("p", "1.2.1");
	hsUrl.addQueryItem("c", "qmn");
	hsUrl.addQueryItem("v", VERSION);
	hsUrl.addQueryItem("u", Config::instance()->lastFmUsername());
	hsUrl.addQueryItem("t", QString::number(time(NULL)));
	hsUrl.addQueryItem("a", getPasswordHash().toHex());

	m_netAccess->get(QNetworkRequest(hsUrl));
	qDebug() << "handshake sent to " << hsUrl.toString();

	m_awaitingHS = true;
}

void LastFmSubmitter::gotNetReply(QNetworkReply * reply) {
	qDebug("gotNetReply...");
	QStringList data = QString(reply->readAll()).split("\n");
	if(data.size()==0)
		return;
	QUrl reqUrl = reply->url();
	reqUrl.setQueryItems(QList<QPair<QString, QString> >());
	qDebug( (QString("reply from ")+reqUrl.toString()).toAscii().data());

	bool handled= false;
	// Is this is a handshake reply?
	if(reqUrl.toString()==m_hsUrl)
	{
		m_awaitingHS = false;
		if(data.size() >= 4 && data[0]=="OK")
		{
			handled = true;
			m_session=data[1];
			m_npUrl=data[2];			
			m_subUrl=data[3];
			qDebug( (QString("hsake result: npurl: ")+m_npUrl+" suburl: "+m_subUrl).toAscii().data());
			if(m_npPending)
				sendNowPlaying();
		} else if(data[0]=="BADAUTH")
		{
			handled = true;
			emit infoMsg(tr("Last.Fm authentication failed: check your credentials"));
		} else if(data[0]=="BADTIME")
		{
			handled = true;
			emit infoMsg(tr("Cannot submit to Last.Fm: system clock is skewed"));
		}
	}
	else
	// Ok... maybe we were scrobbling something?
	if(reqUrl.toString() == m_subUrl)
	{
		if(data[0] == "OK")
		{
			handled = true;
			m_songQueue.clear();
			emit infoMsg(tr("Successfully scrobbled"));
		}
	} else if(reqUrl.toString() == m_npUrl && data[0] == "OK")
	{
		handled=true;
		emit infoMsg(tr("Now Playing sent"));
	}
	// Was i bad player and now there's bad session?
	if(data[0] == "BADSESSION")
	{
		handled = true;
		m_session.clear();
		doHandshake();
	}
	if(data[0].startsWith("FAILED"))
	{
		QStringList dat = data[0].split(" ");
		if(dat.size() >1) emit infoMsg(tr("Last.Fm error: %1").arg(dat.join(" ")));
	}

	if(!handled)
	{
		m_failed++;
		qDebug() << "Failed Reply:" << reqUrl.toString() << data;
	}
	else m_failed=0;

	if(m_failed > 2 && !m_hardFailTimer->isActive())
	{
		m_session.clear();
		m_hardFailTimer->setInterval((m_failed > 120 ? 120 : m_failed)*5*1000);
		m_hardFailTimer->start();
	}
}
