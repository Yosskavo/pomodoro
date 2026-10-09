#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>

#include "TrayIcon.hpp"

class TestTrayIcon : public QObject
{
	Q_OBJECT
	private slots:
		void ignoresNonClickActivations()
		{
			TrayIcon tray;
			QSignalSpy show(&tray, &TrayIcon::showRequested);
			QSignalSpy hide(&tray, &TrayIcon::hideRequested);
			for (auto reason : { QSystemTrayIcon::Unknown, QSystemTrayIcon::Context,
				QSystemTrayIcon::DoubleClick, QSystemTrayIcon::MiddleClick })
				QVERIFY(QMetaObject::invokeMethod(&tray, "onActivated", Qt::DirectConnection,
					Q_ARG(QSystemTrayIcon::ActivationReason, reason)));
			QCoreApplication::processEvents();
			QCOMPARE(show.count(), 0);
			QCOMPARE(hide.count(), 0);
		}

		void restoreFromMenuIsDeferred()
		{
			QApplication::setQuitOnLastWindowClosed(false);
			TrayIcon tray;
			if (!tray.available())
				QSKIP("No system tray on this platform; run the native macOS check as well.");
			QQuickWindow window;
			connect(&tray, &TrayIcon::showRequested, &window, &QWindow::showNormal);
			connect(&tray, &TrayIcon::hideRequested, &window, &QWindow::hide);
			connect(&window, &QWindow::visibleChanged, &tray, &TrayIcon::setWindowVisible);
			tray.setWindowVisible(false);
			tray.setVisible(true);
			auto *icon = tray.findChild<QSystemTrayIcon *>();
			QVERIFY(icon);
			QAction *toggle = icon->contextMenu()->actions().first();
			for (int cycle = 0; cycle < 20; ++cycle)
			{
				toggle->trigger();
				QVERIFY(!window.isVisible());
				QTRY_VERIFY(window.isVisible());
				toggle->trigger();
				QVERIFY(window.isVisible());
				QTRY_VERIFY(!window.isVisible());
			}
		}
};

QTEST_MAIN(TestTrayIcon)
#include "tst_trayicon.moc"
