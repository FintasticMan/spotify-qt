#pragma once

#include "../mainwindow.hpp"
#include "lib/settings.hpp"

#include <QDialog>
#include <QVBoxLayout>
#include <QtGlobal>

class WhatsNewDialog: public QDialog
{
Q_OBJECT

public:
	WhatsNewDialog(const QString &tag, lib::settings &settings, QWidget *parent);

	bool isValid() const;

private:
	bool success = false;
	lib::settings &settings;

	void onDontShowAgain(bool checked);
	void onOk(bool checked);
};
