#pragma once

#include "icon.hpp"
#include "mainwindow.hpp"

#include <QMenu>

class SongMenu : public QMenu
{
public:
	SongMenu(const QString &trackId, const QString &artist, const QString &name,
		const QString &artistId, const QString &albumId, spt::Spotify *spotify, bool remFromPlaylist, QWidget *parent);
};