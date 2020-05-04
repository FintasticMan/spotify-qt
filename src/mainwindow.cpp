#include "mainwindow.hpp"

bool MainWindow::darkBackground	= false;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
	// Some default values to prevent unexpected stuff
	playlists 	= nullptr;
	songs 		= nullptr;
	sptClient	= nullptr;
	volume 		= progress	= nullptr;
	nowPlaying	= position	= nowAlbum	= nullptr;
	repeat 		= shuffle	= playPause	= nullptr;
	isPlaying	= false;
	mediaPlayer	= nullptr;
	artistView	= nullptr;
	lyricsView	= nullptr;
	trayIcon	= nullptr;
	audioFeaturesView	= nullptr;
	playingTrackItem	= nullptr;
	refreshCount 		= -1;
	// Set cache root location
	cacheLocation = QStandardPaths::standardLocations(QStandardPaths::CacheLocation)[0];
	// Create main cache path and album subdir
	QDir cacheDir(cacheLocation);
	cacheDir.mkpath(".");
	cacheDir.mkdir("album");
	cacheDir.mkdir("playlist");
	cacheDir.mkdir("playlistImage");
	// Check for dark background
	auto bg = palette().color(backgroundRole());
	if (((bg.red() + bg.green() + bg.blue()) / 3) < 128)
		darkBackground = true;
	// Set Spotify
	spotify = new spt::Spotify();
	sptPlaylists = new QVector<spt::Playlist>();
	network = new QNetworkAccessManager();
	// Setup main window
	setWindowTitle("spotify-qt");
	setWindowIcon(Icon::get("logo:spotify-qt"));
	resize(1280, 720);
	setCentralWidget(createCentralWidget());
	addToolBar(Qt::ToolBarArea::TopToolBarArea, createToolBar());
	// Apply selected style and palette
	Settings settings;
	QApplication::setStyle(settings.style());
	applyPalette(settings.stylePalette());
	// Update player status
	auto timer = new QTimer(this);
	QTimer::connect(timer, &QTimer::timeout, this, &MainWindow::refresh);
	refresh();
	timer->start(1000);
	// Check if should start client
	if (settings.sptStartClient())
	{
		sptClient = new spt::ClientHandler();
		auto status = sptClient->start();
		if (!status.isEmpty())
			QMessageBox::warning(this,
				"Client error",
				QString("Failed to autostart Spotify client: %1").arg(status));
	}
	// Start media controller if specified
	if (settings.mediaController())
	{
		mediaPlayer = new mp::Service(spotify, this);
		// Check if something went wrong during init
		if (!mediaPlayer->isValid())
		{
			delete mediaPlayer;
			mediaPlayer = nullptr;
		}
	}
	// Start listening to current playback responses
	spt::Spotify::connect(spotify, &spt::Spotify::gotPlayback, [this](const spt::Playback &playback) {
		refreshed(playback);
	});
	// Create tray icon if specified
	if (settings.trayIcon())
		trayIcon = new TrayIcon(spotify, this);
	// Welcome
	setStatus("Welcome to spotify-qt!");
}

MainWindow::~MainWindow()
{
	delete	playlists;
	delete	songs;
	delete	nowPlaying;
	delete	position;
	delete	nowAlbum;
	delete	progress;
	delete	playPause;
	delete	sptPlaylists;
	delete	spotify;
	delete	sptClient;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (trayIcon != nullptr)
		delete trayIcon;
	event->accept();
}

void MainWindow::refresh()
{
	if (refreshCount < 0
		|| ++refreshCount >= Settings().refreshInterval()
		|| current.progressMs + 1000 > current.item.duration)
	{
		spotify->requestCurrentPlayback();
		refreshCount = 0;
		return;
	}
	// Assume last refresh was 1 sec ago
	if (!current.isPlaying)
		return;
	current.progressMs += 1000;
	refreshed(current);
}

void MainWindow::refreshed(const spt::Playback &playback)
{
	current = playback;
	isPlaying = current.isPlaying;
	if (!current.isPlaying)
	{
		playPause->setIcon(Icon::get("media-playback-start"));
		playPause->setText("Play");
		return;
	}
	auto currPlaying = QString("%1\n%2").arg(current.item.name).arg(current.item.artist);
	if (nowPlaying->text() != currPlaying)
	{
		if (current.isPlaying && trackItems.contains(current.item.id))
			setPlayingTrackItem(trackItems[current.item.id]);
		nowPlaying->setText(currPlaying);
		setAlbumImage(current.item.image);
		setWindowTitle(QString("%1 - %2").arg(current.item.artist).arg(current.item.name));
		if (mediaPlayer != nullptr)
			mediaPlayer->currentSourceChanged(current);
	}
	position->setText(QString("%1/%2")
		.arg(formatTime(current.progressMs))
		.arg(formatTime(current.item.duration)));
	progress->setValue(current.progressMs);
	progress->setMaximum(current.item.duration);
	playPause->setIcon(Icon::get(
		current.isPlaying ? "media-playback-pause" : "media-playback-start"));
	playPause->setText(current.isPlaying ? "Pause" : "Play");
	if (!Settings().pulseVolume())
		volume->setValue(current.volume / 5);
	repeat->setChecked(current.repeat != "off");
	shuffle->setChecked(current.shuffle);
}

QGroupBox *createGroupBox(QVector<QWidget*> &widgets)
{
	auto group = new QGroupBox();
	auto layout = new QVBoxLayout();
	for (auto &widget : widgets)
		layout->addWidget(widget);
	group->setLayout(layout);
	return group;
}

QWidget *MainWindow::layoutToWidget(QLayout *layout)
{
	auto widget = new QWidget();
	widget->setLayout(layout);
	return widget;
}

QTreeWidgetItem *MainWindow::treeItem(QTreeWidget *tree, const QString &name, const QString &toolTip, const QStringList &childrenItems)
{
	auto item = new QTreeWidgetItem(tree, {name});
	item->setToolTip(0, toolTip);
	for (auto &child : childrenItems)
		item->addChild(new QTreeWidgetItem(item, {child}));
	return item;
}

QWidget *MainWindow::createCentralWidget()
{
	auto container = new QSplitter();
	// Sidebar with playlists etc.
	auto sidebar = new QVBoxLayout();
	libraryList = new QTreeWidget(this);
	playlists = new QListWidget();
	// Library
	libraryList->addTopLevelItems({
		treeItem(libraryList, "Recently Played", "Most recently played tracks from any device", QStringList()),
		treeItem(libraryList, "Liked", "Liked and saved tracks", QStringList()),
		treeItem(libraryList, "Tracks", "Most played tracks for the past 6 months", QStringList()),
		treeItem(libraryList, "Albums", "Liked and saved albums"),
		treeItem(libraryList, "Artists", "Most played artists for the past 6 months")
	});
	libraryList->header()->hide();
	libraryList->setCurrentItem(nullptr);
	QTreeWidget::connect(libraryList, &QTreeWidget::itemClicked, [this](QTreeWidgetItem *item, int column) {
		if (item != nullptr) {
			playlists->setCurrentRow(-1);
			if (item->parent() != nullptr)
			{
				auto data = item->data(0, 0x100).toString();
				switch (item->data(0, 0x101).toInt())
				{
					case RoleArtistId:	openArtist(data);		break;
					case RoleAlbumId:	loadAlbum(data, false);	break;
				}
			}
			else
			{
				if (item->text(0) == "Recently Played")
					loadSongs(spotify->recentlyPlayed());
				else if (item->text(0) == "Liked")
					loadSongs(spotify->savedTracks());
				else if (item->text(0) == "Tracks")
					loadSongs(spotify->topTracks());
			}
		}
	});
	QTreeWidget::connect(libraryList, &QTreeWidget::itemDoubleClicked, [this](QTreeWidgetItem *item, int column) {
		// Fetch all tracks in list
		auto tracks = item->text(0) == "Recently Played"
			? spotify->recentlyPlayed()
			: item->text(0) == "Liked"
				? spotify->savedTracks()
				: item->text(0) == "Tracks"
					? spotify->topTracks()
					: QVector<spt::Track>();
		// Get id of all tracks
		QStringList trackIds;
		tracks.reserve(tracks.length());
		for (auto &track : tracks)
			trackIds.append(QString("spotify:track:%1").arg(track.id));
		// Play in context of all tracks
		auto status = spotify->playTracks(trackIds.first(), trackIds);
		if (!status.isEmpty())
			setStatus(QString("Failed to start playback: %1").arg(status), true);
	});
	// When expanding top artists, update it
	QTreeWidget::connect(libraryList, &QTreeWidget::itemExpanded, [this](QTreeWidgetItem *item) {
		QVector<QVariantList> results;
		item->takeChildren();

		if (item->text(0) == "Artists")
			for (auto &artist : spotify->topArtists())
				results.append({artist.name, artist.id, RoleArtistId});
		else if (item->text(0) == "Albums")
			for (auto &album : spotify->savedAlbums())
				results.append({album.name, album.id, RoleAlbumId});

		// No results
		if (results.isEmpty())
		{
			auto child = new QTreeWidgetItem(item, {"No results"});
			child->setDisabled(true);
			child->setToolTip(0, "If they should be here, try logging out and back in");
			item->addChild(child);
			return;
		}
		// Add all to the list
		for (auto &result : results)
		{
			auto child = new QTreeWidgetItem(item, {result[0].toString()});
			child->setData(0, 0x100, result[1]);
			child->setData(0, 0x101, result[2]);
			item->addChild(child);
		}
	});
	auto library = createGroupBox(QVector<QWidget*>() << libraryList);
	library->setTitle("Library");
	sidebar->addWidget(library);
	// Update current playlists
	refreshPlaylists();
	// Set default selected playlist
	playlists->setCurrentRow(0);
	QListWidget::connect(playlists, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		if (item != nullptr)
			libraryList->setCurrentItem(nullptr);
		auto currentPlaylist = sptPlaylists->at(playlists->currentRow());
		loadPlaylist(currentPlaylist);
	});
	QListWidget::connect(playlists, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		auto currentPlaylist = sptPlaylists->at(playlists->currentRow());
		loadPlaylist(currentPlaylist);
		auto result = spotify->playTracks(
			QString("spotify:playlist:%1").arg(currentPlaylist.id));
		if (!result.isEmpty())
			setStatus(QString("Failed to start playlist playback: %1").arg(result), true);
	});
	playlists->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);
	QWidget::connect(playlists, &QWidget::customContextMenuRequested, [=](const QPoint &pos) {
		(new PlaylistMenu(*spotify, sptPlaylists->at(playlists->currentRow()), this))
			->popup(playlists->mapToGlobal(pos));
	});
	auto playlistContainer = createGroupBox(QVector<QWidget*>() << playlists);
	playlistContainer->setTitle("Playlists");
	sidebar->addWidget(playlistContainer);
	// Now playing song
	auto nowPlayingLayout = new QHBoxLayout();
	nowPlayingLayout->setSpacing(12);
	nowAlbum = new QLabel();
	nowAlbum->setFixedSize(64, 64);
	nowAlbum->setPixmap(Icon::get("media-optical-audio").pixmap(nowAlbum->size()));
	nowPlayingLayout->addWidget(nowAlbum);
	nowPlaying = new QLabel("No music playing");
	nowPlaying->setWordWrap(true);
	nowPlayingLayout->addWidget(nowPlaying);
	sidebar->addLayout(nowPlayingLayout);
	// Show menu when clicking now playing
	nowPlaying->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);
	QLabel::connect(nowPlaying, &QWidget::customContextMenuRequested, [this](const QPoint &pos) {
		auto track = current.item;
		songMenu(track.id, track.artist, track.name, track.artistId, track.albumId)
			->popup(nowPlaying->mapToGlobal(pos));
	});
	// Sidebar as widget
	auto sidebarWidget = layoutToWidget(sidebar);
	sidebarWidget->setMaximumWidth(250);
	container->addWidget(sidebarWidget);
	// Table with songs
	songs = new QTreeWidget();
	songs->setEditTriggers(QAbstractItemView::NoEditTriggers);
	songs->setSelectionBehavior(QAbstractItemView::SelectRows);
	songs->setSortingEnabled(true);
	songs->setRootIsDecorated(false);
	songs->setAllColumnsShowFocus(true);
	songs->setColumnCount(5);
	songs->setHeaderLabels({
		" ", "Title", "Artist", "Album", "Length", "Added"
	});
	songs->header()->setSectionsMovable(false);
	Settings settings;
	songs->header()->setSectionResizeMode(settings.songHeaderResizeMode());
	if (settings.songHeaderSortBy() > 0)
		songs->header()->setSortIndicator(settings.songHeaderSortBy() + 1, Qt::AscendingOrder);
	// Hide specified columns
	for (auto &value : Settings().hiddenSongHeaders())
		songs->header()->setSectionHidden(value + 1, true);
	// Song context menu
	songs->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);
	QWidget::connect(songs, &QWidget::customContextMenuRequested, [=](const QPoint &pos) {
		auto item = songs->itemAt(pos);
		auto trackId = item->data(0, RoleTrackId).toString();
		if (trackId.isEmpty())
			return;
		songMenu(trackId, item->text(2), item->text(1),
			item->data(0, RoleArtistId).toString(),
			item->data(0, RoleAlbumId).toString())->popup(songs->mapToGlobal(pos));
	});
	QTreeWidget::connect(songs, &QTreeWidget::itemClicked, this, [=](QTreeWidgetItem *item, int column) {
		auto trackId = item->data(0, RoleTrackId).toString();
		if (trackId.isEmpty())
		{
			setStatus("Failed to start playback: track not found", true);
			return;
		}
		// If we played from library, we don't have any context
		auto allTracks = currentTracks();
		auto status = (libraryList->currentItem() != nullptr || !Settings().sptPlaybackOrder())
			&& allTracks.count() < 500
			? spotify->playTracks(trackId, allTracks)
			: spotify->playTracks(trackId, sptContext);

		if (!status.isEmpty())
			setStatus(QString("Failed to start playback: %1").arg(status), true);
		else
			setPlayingTrackItem(item);
		refresh();
	});

	// Songs header context menu
	songs->header()->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);
	QLabel::connect(songs->header(), &QWidget::customContextMenuRequested, [this](const QPoint &pos) {
		auto menu = new QMenu(songs->header());
		auto showHeaders = menu->addMenu(Icon::get("visibility"), "Columns to show");
		auto sortBy = menu->addMenu(Icon::get("view-sort-ascending"), "Default sorting");
		auto headerTitles = QStringList({
			"Title", "Artist", "Album", "Length", "Added"
		});
		Settings settings;
		auto headers = settings.hiddenSongHeaders();
		for (int i = 0; i < headerTitles.size(); i++)
		{
			auto showTitle = showHeaders->addAction(headerTitles.at(i));
			showTitle->setCheckable(true);
			showTitle->setChecked(!headers.contains(i));
			showTitle->setData(QVariant(i));

			auto sortTitle = sortBy->addAction(headerTitles.at(i));
			sortTitle->setCheckable(true);
			sortTitle->setChecked(i == settings.songHeaderSortBy());
			sortTitle->setData(QVariant(100 + i));
		}
		QMenu::connect(menu, &QMenu::triggered, [this](QAction *action) {
			int i = action->data().toInt();
			Settings settings;
			// Columns to show
			if (i < 100)
			{
				songs->header()->setSectionHidden(i + 1, !action->isChecked());
				if (action->isChecked())
					settings.removeHiddenSongHeader(i);
				else
					settings.addHiddenSongHeader(i);
				return;
			}
			// Sort by
			i -= 100;
			if (settings.songHeaderSortBy() != i)
				songs->header()->setSortIndicator(i + 1, Qt::AscendingOrder);
			settings.setSongHeaderSortBy(settings.songHeaderSortBy() == i ? -1 : i);
		});
		menu->popup(songs->header()->mapToGlobal(pos));
	});

	// Load tracks in playlist
	auto playlistId = Settings().lastPlaylist();
	// Default to first in list
	if (playlistId.isEmpty())
		playlistId = sptPlaylists->at(0).id;
	// Find playlist in list
	int i = 0;
	for (auto &playlist : *sptPlaylists)
	{
		if (playlist.id == playlistId)
		{
			playlists->setCurrentRow(i);
			loadPlaylist(playlist);
		}
		i++;
	}
	// Add to main thing
	container->addWidget(songs);
	return container;
}

QMenu *MainWindow::songMenu(const QString &trackId, const QString &artist,
	const QString &name, const QString &artistId, const QString &albumId)
{
	return new SongMenu(trackId, artist, name, artistId, albumId, spotify, this);
}

QToolBar *MainWindow::createToolBar()
{
	auto toolBar = new QToolBar("Media controls", this);
	toolBar->setMovable(false);
	// Menu
	auto menu = new QToolButton(this);
	menu->setText("Menu");
	menu->setIcon(Icon::get("application-menu"));
	menu->setPopupMode(QToolButton::InstantPopup);
	menu->setMenu(new MainMenu(*spotify, this));
	toolBar->addWidget(menu);
	// Search
	search = toolBar->addAction(Icon::get("edit-find"), "Search");
	search->setCheckable(true);
	searchView = new SearchView(*spotify, this);
	addDockWidget(Qt::RightDockWidgetArea, searchView);
	searchView->hide();
	QAction::connect(search, &QAction::triggered, [this](bool checked) {
		if (checked)
			searchView->show();
		else
			searchView->hide();
	});
	// Media controls
	toolBar->addSeparator();
	auto previous = toolBar->addAction(Icon::get("media-skip-backward"), "Previous");
	playPause = toolBar->addAction(Icon::get("media-playback-start"), "Play");
	QAction::connect(playPause, &QAction::triggered, [=](bool checked) {
		current.isPlaying = !current.isPlaying;
		refreshed(current);
		auto status = playPause->iconText() == "Play" ? spotify->pause() : spotify->resume();
		if (!status.isEmpty())
		{
			setStatus(QString("Failed to %1 playback: %2")
				.arg(playPause->iconText() == "Pause" ? "pause" : "resume")
				.arg(status), true);
		}
	});
	auto next = toolBar->addAction(Icon::get("media-skip-forward"),  "Next");
	QAction::connect(previous, &QAction::triggered, [=](bool checked) {
		auto status = spotify->previous();
		if (!status.isEmpty())
			setStatus(QString("Failed to go to previous track: %1").arg(status), true);
		refresh();
	});
	QAction::connect(next, &QAction::triggered, [=](bool checked) {
		auto status = spotify->next();
		if (!status.isEmpty())
			setStatus(QString("Failed to go to next track: %1").arg(status), true);
		refresh();
	});
	// Progress
	progress = new QSlider(this);
	progress->setOrientation(Qt::Orientation::Horizontal);
	QSlider::connect(progress, &QAbstractSlider::sliderReleased, [=]() {
		auto status = spotify->seek(progress->value());
		if (!status.isEmpty())
			setStatus(QString("Failed to seek: %1").arg(status), true);
		if (mediaPlayer != nullptr)
			mediaPlayer->stateUpdated();
	});
	toolBar->addSeparator();
	toolBar->addWidget(progress);
	toolBar->addSeparator();
	position = new QLabel("0:00/0:00", this);
	toolBar->addWidget(position);
	toolBar->addSeparator();
	// Shuffle and repeat toggles
	shuffle = toolBar->addAction(Icon::get("media-playlist-shuffle"), "Shuffle");
	shuffle->setCheckable(true);
	QAction::connect(shuffle, &QAction::triggered, [=](bool checked) {
		current.shuffle = !current.shuffle;
		refreshed(current);
		auto status = spotify->setShuffle(checked);
		if (!status.isEmpty())
			setStatus(QString("Failed to toggle shuffle: %1").arg(status), true);
	});
	repeat = toolBar->addAction(Icon::get("media-playlist-repeat"), "Repeat");
	repeat->setCheckable(true);
	QAction::connect(repeat, &QAction::triggered, [=](bool checked) {
		auto repeatMode = QString(checked ? "context" : "off");
		current.repeat = repeatMode;
		refreshed(current);
		auto status = spotify->setRepeat(repeatMode);
		if (!status.isEmpty())
			setStatus(QString("Failed to toggle repeat: %1").arg(status), true);
	});
	// Volume slider
	volume = new QSlider(this);
	volume->setOrientation(Qt::Orientation::Horizontal);
	volume->setMaximumWidth(100);
	volume->setMinimum(0);
	volume->setMaximum(20);
	volume->setValue(20);
	toolBar->addWidget(volume);
	Settings settings;
	if (settings.pulseVolume())
	{
		// If using PulseAudio for volume control, update on every
		QSlider::connect(volume, &QAbstractSlider::valueChanged, [](int value) {
			QProcess process;
			// Find what sink to use
			process.start("/usr/bin/pactl", {
				"list", "sink-inputs"
			});
			process.waitForFinished();
			auto sinks = QString(process.readAllStandardOutput()).split("Sink Input #");
			QString sink;
			for (auto &s : sinks)
				if (s.contains("Spotify"))
					sink = s;
			if (sink.isEmpty())
				return;
			// Sink was found, get id
			auto left = sink.left(sink.indexOf('\n'));
			auto sinkId = left.right(left.length() - left.lastIndexOf('#') - 1);
			process.start("/usr/bin/pactl", {
				"set-sink-input-volume", sinkId, QString::number(value * 0.05, 'f', 2)
			});
			process.waitForFinished();
		});
	}
	else
	{
		// If using Spotify for volume control, only update on release
		QSlider::connect(volume, &QAbstractSlider::sliderReleased, [=]() {
			auto status = spotify->setVolume(volume->value() * 5);
			if (!status.isEmpty())
				setStatus(QString("Failed to set volume: %1").arg(status), true);
		});
	}
	// Return final tool bar
	return toolBar;
}

void MainWindow::openAudioFeaturesWidget(const QString &trackId, const QString &artist, const QString &name)
{
	auto view = new AudioFeaturesView(*spotify, trackId, artist, name, this);
	if (audioFeaturesView != nullptr)
	{
		audioFeaturesView->close();
		audioFeaturesView->deleteLater();
	}
	audioFeaturesView = view;
	addDockWidget(Qt::DockWidgetArea::RightDockWidgetArea, audioFeaturesView);
}

void MainWindow::openLyrics(const QString &artist, const QString &name)
{
	auto view = new LyricsView(artist, name, this);
	if (!view->lyricsFound())
	{
		view->deleteLater();
		return;
	}
	if (lyricsView != nullptr)
	{
		lyricsView->close();
		lyricsView->deleteLater();
	}
	lyricsView = view;
	addDockWidget(Qt::DockWidgetArea::RightDockWidgetArea, lyricsView);
}

void MainWindow::refreshPlaylists()
{
	spotify->playlists(&sptPlaylists);
	// Create or empty
	if (playlists == nullptr)
		playlists = new QListWidget();
	else
		playlists->clear();
	// Add all playlists
	for (auto &playlist : *sptPlaylists)
		playlists->addItem(playlist.name);
}

bool MainWindow::loadSongs(const QVector<spt::Track> &tracks)
{
	songs->clear();
	trackItems.clear();
	playingTrackItem = nullptr;
	for (int i = 0; i < tracks.length(); i++)
	{
		auto track = tracks.at(i);
		auto item = new QTreeWidgetItem({
			"", track.name, track.artist, track.album,
			formatTime(track.duration), track.addedAt.date().toString(Qt::SystemLocaleShortDate)
		});
		item->setData(0, RoleTrackId,  QString("spotify:track:%1").arg(track.id));
		item->setData(0, RoleArtistId, track.artistId);
		item->setData(0, RoleAlbumId,  track.albumId);
		item->setData(0, RoleIndex,    i);
		if (track.isLocal)
		{
			item->setDisabled(true);
			item->setToolTip(1, "Local track");
		}
		else if (track.id == current.item.id)
			setPlayingTrackItem(item);
		songs->insertTopLevelItem(i, item);
		trackItems[track.id] = item;
	}
	return true;
}

bool MainWindow::loadAlbum(const QString &albumId, bool ignoreEmpty)
{
	auto tracks = spotify->albumTracks(albumId);
	if (ignoreEmpty && tracks->length() <= 1)
		setStatus("Album only contains one song or is empty", true);
	else
	{
		playlists->setCurrentRow(-1);
		libraryList->setCurrentItem(nullptr);
		sptContext = QString("spotify:album:%1").arg(albumId);
		loadSongs(*tracks);
	}
	delete tracks;
	return true;
}

bool MainWindow::loadPlaylist(spt::Playlist &playlist)
{
	Settings().setLastPlaylist(playlist.id);
	if (loadPlaylistFromCache(playlist))
		return true;
	songs->setEnabled(false);
	auto result = loadSongs(playlist.loadTracks(*spotify));
	songs->setEnabled(true);
	sptContext = QString("spotify:playlist:%1").arg(playlist.id);
	if (result)
		cachePlaylist(playlist);
	return result;
}

bool MainWindow::loadPlaylistFromCache(spt::Playlist &playlist)
{
	auto tracks = playlistTracks(playlist.id);
	if (tracks.isEmpty())
		return false;
	songs->setEnabled(false);
	auto result = loadSongs(tracks);
	songs->setEnabled(true);
	sptContext = QString("spotify:playlist:%1").arg(playlist.id);
	refreshPlaylist(playlist);
	return result;
}

QVector<spt::Track> MainWindow::playlistTracks(const QString &playlistId)
{
	QVector<spt::Track> tracks;
	auto filePath = QString("%1/playlist/%2").arg(cacheLocation).arg(playlistId);
	if (!QFileInfo::exists(filePath))
		return tracks;
	QFile file(filePath);
	file.open(QIODevice::ReadOnly);
	auto json = QJsonDocument::fromBinaryData(file.readAll(), QJsonDocument::BypassValidation);
	if (json.isNull())
		return tracks;
	for (auto track : json["tracks"].toArray())
		tracks.append(spt::Track(track.toObject()));
	return tracks;
}

void MainWindow::refreshPlaylist(spt::Playlist &playlist)
{
	auto newPlaylist = spotify->playlist(playlist.id);
	auto tracks = newPlaylist.loadTracks(*spotify);
	if (sptContext.endsWith(playlist.id))
		loadSongs(tracks);
	cachePlaylist(newPlaylist);
}

void MainWindow::setStatus(const QString &message, bool important)
{
	if (trayIcon != nullptr && Settings().trayNotifications())
	{
		if (important)
			trayIcon->message(message);
	}
	else
		statusBar()->showMessage(message, 5000);
}

void MainWindow::setAlbumImage(const QString &url)
{
	nowAlbum->setPixmap(getAlbum(url));
}

QString MainWindow::formatTime(int ms)
{
	auto duration = QTime(0, 0).addMSecs(ms);
	return QString("%1:%2")
		.arg(duration.minute())
		.arg(duration.second() % 60, 2, 10, QChar('0'));
}

QByteArray MainWindow::get(const QString &url)
{
	auto reply = network->get(QNetworkRequest(QUrl(url)));
	while (!reply->isFinished())
		QCoreApplication::processEvents();
	reply->deleteLater();
	return reply->readAll();
}

QJsonDocument MainWindow::getJson(const QString &url)
{
	return QJsonDocument::fromJson(get(url));
}

QPixmap MainWindow::getImage(const QString &type, const QString &url)
{
	QPixmap img;
	// Check if cache exists
	auto cachePath = QString("%1/%2/%3").arg(cacheLocation).arg(type).arg(QFileInfo(url).baseName());
	if (QFileInfo::exists(cachePath))
	{
		// Read file from cache
		QFile file(cachePath);
		file.open(QIODevice::ReadOnly);
		img.loadFromData(file.readAll(), "jpeg");
	}
	else
	{
		// Download image and save to cache
		img.loadFromData(get(url), "jpeg");
		if (!img.save(cachePath, "jpeg"))
			qDebug() << "failed to save album cache to" << cachePath;
	}
	return img;
}

QPixmap MainWindow::getAlbum(const QString &url)
{
	return getImage("album", url);
}

void MainWindow::openArtist(const QString &artistId)
{
	auto view = new ArtistView(spotify, artistId, this);
	if (artistView != nullptr)
	{
		artistView->close();
		artistView->deleteLater();
	}
	artistView = view;
	addDockWidget(Qt::DockWidgetArea::RightDockWidgetArea, artistView);
	libraryList->setCurrentItem(nullptr);
}

void MainWindow::cachePlaylist(spt::Playlist &playlist)
{
	QJsonDocument json(playlist.toJson(*spotify));
	QFile file(QString("%1/playlist/%2").arg(cacheLocation).arg(playlist.id));
	file.open(QIODevice::WriteOnly);
	file.write(json.toBinaryData());
}

void MainWindow::applyPalette(Settings::Palette palette)
{
	QPalette p;
	switch (palette)
	{
		case Settings::paletteApp:		p = QApplication::palette(); 					break;
		case Settings::paletteStyle:	p = QApplication::style()->standardPalette();	break;
		case Settings::paletteDark:		p = DarkPalette();								break;
	}
	QApplication::setPalette(p);
}

QStringList MainWindow::currentTracks()
{
	QStringList tracks;
	tracks.reserve(songs->topLevelItemCount());
	for (int i = 0; i < songs->topLevelItemCount(); i++)
	{
		auto trackId = songs->topLevelItem(i)->data(0, RoleTrackId).toString();
		// spotify:track: = 14
		if (trackId.length() > 14)
			tracks.append(trackId);
	}
	return tracks;
}

spt::Playback MainWindow::currentPlayback()
{
	return current;
}

bool MainWindow::hasPlaylistSelected()
{
	return playlists->currentRow() >= 0;
}

QString MainWindow::currentLibraryItem()
{
	return libraryList->currentIndex().row() >= 0
		? libraryList->currentItem()->text(0)
		: QString();
}

void MainWindow::reloadTrayIcon()
{
	if (trayIcon != nullptr)
	{
		delete trayIcon;
		trayIcon = nullptr;
	}
	if (Settings().trayIcon())
		trayIcon = new TrayIcon(spotify, this);
}

bool MainWindow::hasDarkBackground()
{
	return darkBackground;
}

void MainWindow::setPlayingTrackItem(QTreeWidgetItem *item)
{
	if (playingTrackItem != nullptr)
		playingTrackItem->setIcon(0, QIcon());
	if (item == nullptr)
	{
		playingTrackItem = nullptr;
		return;
	}
	item->setIcon(0, Icon::get("media-playback-start"));
	playingTrackItem = item;
}
