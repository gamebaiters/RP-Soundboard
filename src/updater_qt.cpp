// src/updater_qt.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//
// See updater_qt.h for the v2.3.3 rework notes (custom-painted bar,
// verbose stages, robust TeamSpeak close in the update helper).
//----------------------------------


#include "buildinfo.h"
#include "updater_qt.h"
#include "style_helper.h"
#include "ts3log.h"
#include "plugin.h"
#include <QMessageBox>
#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QNetworkAccessManager>

#include <algorithm>


// Custom-painted determinate progress bar (same pattern as the export
// dialog's ExportBar). A QSS-styled QProgressBar on the Windows native
// style repaints its chunk unreliably — users with font scaling /
// ClearType tweaks reported bars stuck at 0%, fill leaking outside the
// rounded groove, or the terminal state never rendering. Painting the
// track + fill ourselves removes the style round-trip entirely.
namespace {

class UpdaterBar : public QWidget {
public:
	explicit UpdaterBar(QWidget *parent = nullptr) : QWidget(parent) {
		setMinimumHeight(16);
		setMaximumHeight(16);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		m_anim = new QTimer(this);
		m_anim->setInterval(16);   // 60 Hz easing
		QObject::connect(m_anim, &QTimer::timeout, this, [this]{ tick(); });
	}

	void setTarget(qreal v) {
		v = std::max<qreal>(0.0, std::min<qreal>(1.0, v));
		if (qFuzzyCompare(m_target, v)) return;
		m_target = v;
		if (!m_anim->isActive()) m_anim->start();
	}

	void snapTo(qreal v) {
		v = std::max<qreal>(0.0, std::min<qreal>(1.0, v));
		m_target = v;
		m_drawn  = v;
		m_anim->stop();
		update();
	}

	void setFillColor(const QColor &c) { m_fill = c; update(); }

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		const qreal r = height() / 2.0;
		const QRectF full(0.5, 0.5, width() - 1.0, height() - 1.0);
		QPainterPath base;
		base.addRoundedRect(full, r, r);
		p.fillPath(base, m_track);
		p.setPen(QPen(m_border, 1.0));
		p.setBrush(Qt::NoBrush);
		p.drawPath(base);
		if (m_drawn > 0.0) {
			p.save();
			p.setClipPath(base);
			p.setPen(Qt::NoPen);
			p.setBrush(m_fill);
			p.drawRect(QRectF(full.left(), full.top(),
			                  full.width() * m_drawn, full.height()));
			p.restore();
		}
	}

private:
	void tick() {
		constexpr qreal kSlew = 0.22;
		const qreal diff = m_target - m_drawn;
		if (std::abs(diff) < 1e-4) {
			m_drawn = m_target;
			m_anim->stop();
		} else {
			m_drawn += diff * kSlew;
		}
		update();
	}

	qreal   m_target = 0.0;
	qreal   m_drawn  = 0.0;
	QTimer *m_anim   = nullptr;
	QColor  m_fill   = QColor(0x3f, 0xa7, 0xff);
	QColor  m_track  = QColor(38, 38, 48);
	QColor  m_border = QColor(70, 70, 82);
};

static QString humanSize(qint64 bytes)
{
	if (bytes < 0) return QStringLiteral("?");
	const double mb = (double)bytes / (1024.0 * 1024.0);
	return QString::number(mb, 'f', 1) + " MB";
}

} // namespace


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
UpdaterWindow::UpdaterWindow( QWidget *parent /*= 0*/ ) :
	QDialog(parent),
	m_file(NULL),
	m_manager(NULL),
	m_reply(NULL),
	m_redirects(0),
	m_execute(false),
	m_canceled(false),
	m_success(false)
{
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	setWindowTitle(tr("Soundboard update"));
	setMinimumWidth(460);
	this->setStyleSheet(StyleHelper::loadDarkStyle());

	m_titleLabel = new QLabel(tr("Updating GameBaiters Soundboard"), this);
	{
		QFont f = m_titleLabel->font();
		f.setPointSize(12);
		f.setBold(true);
		m_titleLabel->setFont(f);
	}

	m_statusLabel = new QLabel(tr("Preparing…"), this);

	auto *bar = new UpdaterBar(this);
	m_bar = bar;

	m_log = new QPlainTextEdit(this);
	m_log->setReadOnly(true);
	m_log->setMaximumHeight(110);
	m_log->setFrameShape(QFrame::StyledPanel);
	{
		QFont f = m_log->font();
		f.setPointSize(8);
		m_log->setFont(f);
	}

	m_cancelBtn = new QPushButton(tr("Cancel"), this);
	m_cancelBtn->setMinimumWidth(96);
	connect(m_cancelBtn, &QPushButton::clicked,
	        this, &UpdaterWindow::onClickedCancel);

	auto *btnRow = new QHBoxLayout;
	btnRow->addStretch(1);
	btnRow->addWidget(m_cancelBtn);

	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(16, 14, 16, 12);
	lay->setSpacing(10);
	lay->addWidget(m_titleLabel);
	lay->addWidget(m_statusLabel);
	lay->addWidget(bar);
	lay->addWidget(m_log);
	lay->addLayout(btnRow);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void UpdaterWindow::appendLog(const QString &line)
{
	if (m_log) m_log->appendPlainText(line);
	logInfo("[updater] %s", line.toUtf8().constData());
}

void UpdaterWindow::setStatus(const QString &text)
{
	if (m_statusLabel) m_statusLabel->setText(text);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void UpdaterWindow::startDownload(const QUrl &url, const QFileInfo &fileInfo, bool execute /*= false*/)
{
	m_url = url;
	m_fileinfo = fileInfo;
	m_execute = execute;

	if(m_fileinfo.fileName().isEmpty())
	{
		QFileInfo info(url.path());
		m_fileinfo.setFile(info.fileName());
	}

	appendLog(tr("Update package: %1").arg(m_fileinfo.fileName()));
	appendLog(tr("Downloading from %1").arg(m_url.host()));
	logInfo("Downloading update from '%s' to '%s'", m_url.toString().toUtf8().data(), m_fileinfo.absoluteFilePath().toUtf8().data());

	m_file = new QFile(m_fileinfo.filePath());
	if(!m_file->open(QIODevice::WriteOnly))
	{
		logError("Unable to write to file %s", m_fileinfo.absoluteFilePath().toUtf8().data());
		appendLog(tr("ERROR: cannot write to %1").arg(m_fileinfo.absoluteFilePath()));
		QMessageBox::information(this, tr("Error"), tr("Unable to save the file."));
		delete m_file;
		m_file = NULL;
		return;
	}

	setStatus(tr("Connecting…"));
	m_speedTimer.start();
	m_lastBytes = 0;
	m_speedBps  = 0.0;
	m_manager = new QNetworkAccessManager(this);
	startRequest(url);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void UpdaterWindow::startRequest(const QUrl &url)
{
	QNetworkRequest request(url);
	request.setRawHeader("User-Agent", QByteArray("RP Soundboard Updater, ") + buildinfo_getPluginVersion());
	m_reply = m_manager->get(QNetworkRequest(url));
	connect(m_reply, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
	connect(m_reply, SIGNAL(downloadProgress(qint64,qint64)), this,
		SLOT(onDownloadProgress(qint64, qint64)));
	connect(m_reply, SIGNAL(finished()), this, SLOT(onFinished()));
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void UpdaterWindow::onReadyRead()
{
	if(m_file)
		m_file->write(m_reply->readAll());
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void UpdaterWindow::onDownloadProgress(qint64 bytes, qint64 total)
{
	if(m_canceled)
		return;

	// Rolling speed estimate (updated at most ~2x/s so the label is stable).
	const qint64 elapsedMs = m_speedTimer.elapsed();
	if (elapsedMs >= 500) {
		m_speedBps = (double)(bytes - m_lastBytes) * 1000.0 / (double)elapsedMs;
		m_lastBytes = bytes;
		m_speedTimer.restart();
	}
	const QString speed = (m_speedBps > 1024.0)
		? QString(" — %1/s").arg(humanSize((qint64)m_speedBps))
		: QString();

	if (auto *bar = static_cast<UpdaterBar*>(m_bar)) {
		if (total > 0) {
			bar->setTarget((qreal)bytes / (qreal)total);
			const int pct = (int)(100.0 * (double)bytes / (double)total);
			setStatus(tr("Downloading update… %1% (%2 of %3)%4")
			          .arg(pct).arg(humanSize(bytes), humanSize(total), speed));
		} else {
			// Unknown size: show what we have (bar stays where it is).
			setStatus(tr("Downloading update… %1%2")
			          .arg(humanSize(bytes), speed));
		}
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void UpdaterWindow::onFinished()
{
	if(m_canceled)
	{
		logDebug("Canceled download of update");
		appendLog(tr("Download cancelled."));
		m_canceled = false;
		if(m_file)
		{
			m_file->close();
			m_file->remove();
			delete m_file;
			m_file = NULL;
		}
		m_reply->deleteLater();
		m_reply = NULL;
		this->hide();
	}
	else
	{
		m_file->flush();
		m_file->close();

		QVariant redirect = m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
		if(m_reply->error())
		{
			m_file->remove();
			appendLog(tr("ERROR: download failed — %1").arg(m_reply->errorString()));
			setStatus(tr("Download failed."));
			QMessageBox::information(this, tr("Error"), tr("Could not download the update: %1").arg(m_reply->errorString()));
		}
		else if(redirect.isValid())
		{
			if(m_redirects < 10)
			{
				m_redirects++;
				QUrl url = m_url.resolved(redirect.toUrl());
				logInfo("Download of update redirected to %s", url.toString().toUtf8().data());
				appendLog(tr("Following redirect (%1)…").arg(m_redirects));
				m_url = url;
				m_reply->deleteLater();
				m_reply = NULL;
				m_file->open(QIODevice::WriteOnly);
				m_file->resize(0);
				startRequest(url);
				return;
			}
			else
			{
				logError("Download of update redirected more than 10 times, maybe a redirection loop?");
				appendLog(tr("ERROR: too many redirects."));
				m_success = false;
			}
		}
		else
		{
			if (auto *bar = static_cast<UpdaterBar*>(m_bar)) {
				bar->setFillColor(QColor(0x4c, 0xaf, 0x50));
				bar->snapTo(1.0);
			}
			// Fresh QFileInfo: m_fileinfo was created before the download and
			// caches a stale (zero) size.
			appendLog(tr("Download complete (%1).")
			          .arg(humanSize(QFileInfo(m_fileinfo.filePath()).size())));
			m_success = true;
			if(m_execute)
			{
				setStatus(tr("Starting the installer — TeamSpeak will now close…"));
				appendLog(tr("Launching the update helper. TeamSpeak will close, "
				             "the new version will be installed, then you can "
				             "start TeamSpeak again."));
				m_success = executeFile();
				if (!m_success)
					appendLog(tr("ERROR: could not start the update helper."));
			}
			this->hide();
		}

		m_reply->deleteLater();
		m_reply = NULL;
		delete m_file;
		m_file = NULL;
		m_manager = NULL;
		m_redirects = 0;
	}

	emit finished();
}


//---------------------------------------------------------------
// Purpose: write + launch the platform update helper. The Windows helper is
// deliberately VERBOSE (visible console with numbered steps) and now waits
// for TeamSpeak to exit GRACEFULLY (up to ~20 s) before force-killing it.
// The old helper taskkill /F'd after 2 s flat, which could kill the client
// mid-shutdown (settings write, plugin unload) — that half-written state is
// what made TS3 show its "crashed last time" dialog on the next start, and
// on slow machines the installer even raced a still-alive ts3client with the
// plugin DLL locked. The delete is retried too, for the same reason.
//---------------------------------------------------------------
bool UpdaterWindow::executeFile()
{
#ifdef _WIN32
	QString helperPath = QDir::temp().absoluteFilePath("rpsb_update_helper.bat");
	QFile helper(helperPath);
	if (!helper.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		logError("Could not write update helper to %s", helperPath.toUtf8().data());
		return false;
	}
	QString tplugin = QDir::toNativeSeparators(m_fileinfo.absoluteFilePath());
	const char *cfgDir = getTs3ConfigPath();
	QString pluginsDir;
	if (cfgDir && cfgDir[0])
		pluginsDir = QDir::toNativeSeparators(QString::fromUtf8(cfgDir) + "plugins");
	else
		pluginsDir = "%APPDATA%\\TS3Client\\plugins";
	QTextStream out(&helper);
	out << "@echo off\r\n";
	out << "REM GameBaiters Soundboard auto-update helper (generated at runtime)\r\n";
	out << "title GameBaiters Soundboard - Update\r\n";
	out << "echo ==============================================\r\n";
	out << "echo   GameBaiters Soundboard automatic update\r\n";
	out << "echo ==============================================\r\n";
	out << "echo.\r\n";
	// [1/4] Wait for a GRACEFUL TeamSpeak exit first (the plugin asked the
	// client to close via closeAllWindows). Killing it early corrupts its
	// shutdown (settings write, plugin unload) and triggers the "TeamSpeak
	// crashed" dialog on the next start.
	out << "echo [1/4] Waiting for TeamSpeak to close (this can take a few seconds)...\r\n";
	out << "set tries=0\r\n";
	out << ":waitts\r\n";
	out << "tasklist /FI \"IMAGENAME eq ts3client_win64.exe\" 2>nul | find /I \"ts3client_win64.exe\" >nul\r\n";
	out << "if not errorlevel 1 goto tsalive\r\n";
	out << "tasklist /FI \"IMAGENAME eq ts3client_win32.exe\" 2>nul | find /I \"ts3client_win32.exe\" >nul\r\n";
	out << "if errorlevel 1 goto tsdead\r\n";
	out << ":tsalive\r\n";
	out << "set /a tries+=1\r\n";
	out << "if %tries% GEQ 20 goto forcekill\r\n";
	out << "timeout /t 1 /nobreak >nul\r\n";
	out << "goto waitts\r\n";
	out << ":forcekill\r\n";
	out << "echo        TeamSpeak did not close by itself - closing it now...\r\n";
	// Polite close first (WM_CLOSE), force only as the very last resort.
	out << "taskkill /IM ts3client_win64.exe >nul 2>&1\r\n";
	out << "taskkill /IM ts3client_win32.exe >nul 2>&1\r\n";
	out << "timeout /t 4 /nobreak >nul\r\n";
	out << "taskkill /F /IM ts3client_win64.exe >nul 2>&1\r\n";
	out << "taskkill /F /IM ts3client_win32.exe >nul 2>&1\r\n";
	out << "timeout /t 2 /nobreak >nul\r\n";
	out << ":tsdead\r\n";
	out << "echo        TeamSpeak is closed.\r\n";
	out << "echo.\r\n";
	// [2/4] Remove the old plugin, retrying while Windows releases the DLL
	// lock (can lag a second or two behind process exit).
	out << "echo [2/4] Removing the old Soundboard plugin...\r\n";
	out << "del /F /Q \"" << pluginsDir << "\\rp_soundboard_fx_win32.dll\" >nul 2>&1\r\n";
	out << "del /F /Q \"" << pluginsDir << "\\rp_soundboard_win64.dll\" >nul 2>&1\r\n";
	out << "del /F /Q \"" << pluginsDir << "\\rp_soundboard_win32.dll\" >nul 2>&1\r\n";
	out << "del /F /Q \"" << pluginsDir << "\\rp_soundboard.dll\" >nul 2>&1\r\n";
	out << "set delTries=0\r\n";
	out << ":delloop\r\n";
	out << "del /F /Q \"" << pluginsDir << "\\rp_soundboard_fx_win64.dll\" >nul 2>&1\r\n";
	out << "if not exist \"" << pluginsDir << "\\rp_soundboard_fx_win64.dll\" goto deldone\r\n";
	out << "set /a delTries+=1\r\n";
	out << "if %delTries% GEQ 10 goto delfail\r\n";
	out << "echo        Plugin file still locked - retrying...\r\n";
	out << "timeout /t 1 /nobreak >nul\r\n";
	out << "goto delloop\r\n";
	out << ":delfail\r\n";
	out << "echo        WARNING: could not remove the old plugin file.\r\n";
	out << "echo        The installer will try to overwrite it.\r\n";
	out << ":deldone\r\n";
	out << "echo        Done.\r\n";
	out << "echo.\r\n";
	out << "echo [3/4] Starting the plugin installer...\r\n";
	out << "start \"\" \"" << tplugin << "\"\r\n";
	out << "echo.\r\n";
	out << "echo [4/4] Follow the TeamSpeak plugin installer window, then start\r\n";
	out << "echo        TeamSpeak again. This window closes in 8 seconds.\r\n";
	out << "timeout /t 8 >nul\r\n";
	helper.close();

	bool status = QProcess::startDetached("cmd.exe",
		QStringList() << "/c" << helperPath);
	if (!status)
	{
		logError("Could not start update helper at %s", helperPath.toUtf8().data());
		return false;
	}
	return true;
#elif defined(__linux__) || defined(__APPLE__)
	QString helperPath = QDir::temp().absoluteFilePath("rpsb_update_helper.sh");
	QFile helper(helperPath);
	if (!helper.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		logError("Could not write update helper to %s", helperPath.toUtf8().data());
		return false;
	}
	QString tplugin = m_fileinfo.absoluteFilePath();
	QTextStream out(&helper);
	out << "#!/usr/bin/env bash\n";
	out << "# GameBaiters Soundboard auto-update helper (generated at runtime)\n";
	out << "set -u\n";
	out << "PACKAGE='" << tplugin << "'\n";
	out << "sleep 2\n";
#ifdef __APPLE__
	out << "osascript -e 'tell application \"TeamSpeak 3\" to quit' 2>/dev/null || true\n";
	// Wait for a GRACEFUL exit (up to 15 s) before killing: SIGKILL during
	// the client's own shutdown is what produced the "crashed last time"
	// dialog on the next start.
	out << "for i in $(seq 1 15); do\n";
	out << "    pgrep -x ts3client >/dev/null 2>&1 || break\n";
	out << "    sleep 1\n";
	out << "done\n";
	out << "pkill -9 -x ts3client 2>/dev/null || true\n";
	out << "pkill -9 -if 'TeamSpeak 3' 2>/dev/null || true\n";
	out << "sleep 1\n";
	// Resolve the actual TS3 plugin folder. macOS users typically have
	// "~/Library/Application Support/TeamSpeak 3" (note the space). Fallback
	// chain matches the install_embedded_macos.sh shipped with Install
	// Soundboard.app so manual + auto installs converge to the same path.
	out << "TARGET_BASE=''\n";
	out << "for cand in \"$HOME/Library/Application Support/TeamSpeak 3\" \\\n";
	out << "            \"$HOME/Library/Application Support/TS3Client\" \\\n";
	out << "            \"$HOME/.ts3client\"; do\n";
	out << "    if [ -d \"$cand\" ]; then TARGET_BASE=\"$cand\"; break; fi\n";
	out << "done\n";
	out << "[ -z \"$TARGET_BASE\" ] && TARGET_BASE=\"$HOME/Library/Application Support/TeamSpeak 3\"\n";
	out << "PLUGIN_DIR=\"$TARGET_BASE/plugins\"\n";
	out << "mkdir -p \"$PLUGIN_DIR\"\n";
	// Remove every prior macOS variant + every bundled dep dylib so
	// stale FFmpeg / transitive libs from the previous install don't
	// linger next to the fresh ones (preserve rp_soundboard.ini).
	out << "for lib in librp_soundboard_fx_mac.dylib librp_soundboard_fx_mac.so \\\n";
	out << "           rp_soundboard_fx_mac.dylib    rp_soundboard_fx_mac.so   \\\n";
	out << "           librp_soundboard_fx.dylib     rp_soundboard_fx.dylib    \\\n";
	out << "           librp_soundboard.dylib        libsoundboard.dylib; do\n";
	out << "    rm -f \"$PLUGIN_DIR/$lib\" 2>/dev/null\n";
	out << "done\n";
	out << "rm -f \"$PLUGIN_DIR\"/lib*.dylib 2>/dev/null || true\n";
	// Direct extraction: TS3.app on macOS does not ship a separate
	// package_inst binary, and `open` depends on a fragile file association.
	// `ditto -x -k` extracts a renamed-zip .ts3_plugin reliably.
	out << "TMP=\"$(mktemp -d)\"\n";
	out << "trap 'rm -rf \"$TMP\"' EXIT\n";
	out << "DITTO_OK=1\n";
	out << "/usr/bin/ditto -x -k \"$PACKAGE\" \"$TMP\" 2>/tmp/rpsb_update.err || DITTO_OK=0\n";
	out << "if [ \"$DITTO_OK\" = \"1\" ] && [ -d \"$TMP/plugins\" ]; then\n";
	out << "    cp -R \"$TMP/plugins/.\" \"$PLUGIN_DIR/\"\n";
	// Clear every xattr recursively (quarantine + provenance). Whitelisting
	// by filename misses bundled transitive deps like libssl/libcrypto/...
	out << "    xattr -cr \"$PLUGIN_DIR\" 2>/dev/null || true\n";
	// Re-sign ad-hoc each Mach-O so Gatekeeper accepts the fresh deps
	out << "    find \"$PLUGIN_DIR\" -maxdepth 1 -name '*.dylib' -print0 2>/dev/null | xargs -0 -I {} codesign --force -s - {} 2>/dev/null || true\n";
	out << "    INSTALL_OK=1\n";
	out << "else\n";
	out << "    INSTALL_OK=0\n";
	out << "fi\n";
	// Relaunch TS3 so the user does not have to reopen the client manually.
	// `open -a` queues the launch even if the previous instance is still
	// quitting - macOS serialises by bundle identifier.
	out << "sleep 1\n";
	out << "open -a 'TeamSpeak 3' 2>/dev/null || open -a 'TeamSpeak 3 Client' 2>/dev/null || true\n";
	// Surface a native dialog so the user can confirm the install
	// without having to dig through TS3 to check whether the plugin
	// reloaded. Silent on success-then-relaunch was confusing.
	out << "if [ \"$INSTALL_OK\" = \"1\" ]; then\n";
	out << "    osascript -e 'display notification \"Soundboard updated. TeamSpeak is restarting.\" with title \"GameBaiters Soundboard\"' 2>/dev/null || true\n";
	out << "else\n";
	out << "    ERRMSG=\"$(cat /tmp/rpsb_update.err 2>/dev/null | head -c 200)\"\n";
	out << "    osascript -e \"display dialog \\\"Soundboard auto-update failed: $ERRMSG. Please install $PACKAGE manually.\\\" buttons {\\\"OK\\\"}\" 2>/dev/null || true\n";
	out << "fi\n";
#else
	// Graceful first (SIGTERM), wait up to 15 s, then SIGKILL — same
	// don't-kill-a-client-mid-shutdown rationale as the other platforms.
	out << "pkill -x ts3client_linux_amd64 2>/dev/null || true\n";
	out << "pkill -x ts3client_linux_x86 2>/dev/null || true\n";
	out << "for i in $(seq 1 15); do\n";
	out << "    pgrep -x ts3client_linux_amd64 >/dev/null 2>&1 || pgrep -x ts3client_linux_x86 >/dev/null 2>&1 || break\n";
	out << "    sleep 1\n";
	out << "done\n";
	out << "pkill -9 -x ts3client_linux_amd64 2>/dev/null || true\n";
	out << "pkill -9 -x ts3client_linux_x86 2>/dev/null || true\n";
	out << "sleep 1\n";
	out << "for base in \"$HOME/.ts3client\" \"$HOME/Library/Application Support/TS3Client\"; do\n";
	out << "    [ -d \"$base/plugins\" ] || continue\n";
	out << "    for lib in \\\n";
	out << "        librp_soundboard_fx.so   librp_soundboard.so   libsoundboard.so   \\\n";
	out << "        rp_soundboard_fx.so      rp_soundboard.so      soundboard.so      \\\n";
	out << "        librp_soundboard_fx_linux_amd64.so librp_soundboard_linux_amd64.so; do\n";
	out << "        rm -f \"$base/plugins/$lib\" 2>/dev/null\n";
	out << "    done\n";
	out << "done\n";
	out << "if command -v package_inst >/dev/null 2>&1; then\n";
	out << "    package_inst \"$PACKAGE\"\n";
	out << "elif command -v xdg-open >/dev/null 2>&1; then\n";
	out << "    xdg-open \"$PACKAGE\"\n";
	out << "else\n";
	out << "    echo \"package_inst not found in PATH; install manually: $PACKAGE\" >&2\n";
	out << "fi\n";
#endif
	helper.close();

	helper.setPermissions(helper.permissions()
		| QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther);

	bool status = QProcess::startDetached("/bin/bash",
		QStringList() << helperPath);
	if (!status)
	{
		logError("Could not start update helper at %s", helperPath.toUtf8().data());
		return false;
	}
	return true;
#else
	bool status = QProcess::startDetached("package_inst",
		QStringList(m_fileinfo.absoluteFilePath()));
	if(!status)
		logError("Error starting package_inst with cmd line \"%s\"",
			m_fileinfo.absoluteFilePath().toUtf8().data());
	return status;
#endif
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void UpdaterWindow::onClickedCancel()
{
	if(!m_canceled && m_reply)
	{
		logDebug("Cancelling download of update...");
		m_canceled = true;
		setStatus(tr("Cancelling download…"));
		appendLog(tr("Cancelling…"));
		m_reply->abort();
	}
}
