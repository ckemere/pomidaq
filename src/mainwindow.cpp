/*
 * Copyright (C) 2019 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the license, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this software.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDebug>
#include <QScrollBar>
#include <QMessageBox>
#include <QLabel>
#include <miniscope.h>
#include <QStandardPaths>
#include <QFileDialog>
#include <QDateTime>
#include <QMessageBox>
#include "videoviewwidget.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // Create status bar
    m_statusBarLabel = new QLabel("OK", this);
    statusBar()->addWidget(m_statusBarLabel, 1);

    // don't display log by default
    ui->logTextList->setVisible(false);

    // Video view
    m_scopeView = new VideoViewWidget(this);
    ui->videoDisplayWidget->layout()->addWidget(m_scopeView);

    m_mscope = new MiniScope();
    m_mscope->setOnMessage([&](const std::string &msg) {
        m_newMessages.enqueue(QString::fromStdString(msg));
    });

    // display default values
    ui->sbExposure->setValue(static_cast<int>(m_mscope->exposure()));
    ui->sbExcitation->setValue(m_mscope->excitation());
    ui->sbGain->setValue(static_cast<int>(m_mscope->gain()));
    ui->accAlphaSpinBox->setValue(m_mscope->bgAccumulateAlpha());

    ui->btnStartStop->setFocus();
    ui->containerScopeControls->setEnabled(false);
    ui->groupBoxDisplay->setEnabled(false);
    ui->btnRecord->setEnabled(false);

    // ensure codecs and container UI is aligned with the MiniScope settings
    ui->codecComboBox->setCurrentIndex(0);
    this->on_codecComboBox_currentIndexChanged(ui->codecComboBox->currentText());
    ui->containerComboBox->setCurrentIndex(0);
    this->on_containerComboBox_currentIndexChanged(ui->containerComboBox->currentText());
    ui->losslessCheckBox->setChecked(true);
    on_sliceIntervalSpinBox_valueChanged(ui->sliceIntervalSpinBox->value());

    // set export directory, default to /tmp
    setDataExportDir(QStandardPaths::writableLocation(QStandardPaths::StandardLocation::TempLocation));
    if (dataDir.isEmpty())
        setDataExportDir("/tmp");
}

MainWindow::~MainWindow()
{
    delete ui;
    delete m_mscope;
}

void MainWindow::on_sbExcitation_valueChanged(double arg1)
{
    arg1 = round(arg1 * 100) / 100;
    m_mscope->setExcitation(arg1);

    double intpart;
    if (std::modf(arg1, &intpart) == 0.0)
        ui->dialExcitation->setValue(static_cast<int>(arg1));
}

void MainWindow::on_dialExcitation_valueChanged(int value)
{
    ui->sbExcitation->setValue(value);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    m_mscope->disconnect();
}

void MainWindow::addLogMessage(const QString &msg)
{
    m_messageCount += 1;
    if (m_messageCount > 200) {
        m_messageCount = 1;
        ui->logTextList->clear();
    }

    ui->logTextList->appendPlainText(msg);
    ui->logTextList->verticalScrollBar()->setValue(ui->logTextList->verticalScrollBar()->maximum());
    setStatusText(msg);
}

void MainWindow::setStatusText(const QString& msg)
{
    m_statusBarLabel->setText(msg);
    QApplication::processEvents();
}

void MainWindow::setDataExportDir(const QString &dir)
{
    setWindowTitle(QStringLiteral("Portable Miniscope DAQ - %1").arg(dir));
    dataDir = dir;
}

void MainWindow::on_sbExposure_valueChanged(int arg1)
{
    m_mscope->setExposure(arg1);
}

void MainWindow::on_btnStartStop_clicked()
{
    if (m_mscope->running()) {
        ui->btnStartStop->setEnabled(false);
        QApplication::processEvents();
        m_mscope->disconnect();
        ui->btnStartStop->setEnabled(true);
        ui->sbCamId->setEnabled(true);
        return;
    }
    m_newMessages.clear();

    ui->btnStartStop->setEnabled(false);
    m_mscope->setScopeCamId(ui->sbCamId->value());
    if (!m_mscope->connect()) {
        QMessageBox::critical(this,
                              "Error",
                              QString("Unable to connect to camera '%1'.").arg(ui->sbCamId->value()));
        setStatusText("Connection error.");
        ui->btnStartStop->setEnabled(true);
        return;
    }

    // run and display images
    m_mscope->run();

    ui->btnStartStop->setText("Disconnect");
    ui->btnStartStop->setChecked(true);
    ui->containerScopeControls->setEnabled(true);
    ui->groupBoxDisplay->setEnabled(true);
    ui->btnRecord->setEnabled(true);
    ui->btnStartStop->setEnabled(true);
    ui->sbCamId->setEnabled(false);

    while (m_mscope->running()) {
        auto frame = m_mscope->currentFrame();
        if (!frame.empty()) {
            m_scopeView->showImage(frame);

            ui->labelCurrentFPS->setText(QString::number(m_mscope->currentFPS()));
            ui->labelDroppedFrames->setText(QString::number(m_mscope->droppedFramesCount()));

            ui->labelScopeMin->setText(QString::number(m_mscope->minFluor()).rightJustified(3, '0'));
            ui->labelScopeMax->setText(QString::number(m_mscope->maxFluor()).rightJustified(3, '0'));

            const auto recMsecTimestamp = static_cast<int>(m_mscope->lastRecordedFrameTime()); // cast a double to an int
            ui->labelRecordingTime->setText(QTime::fromMSecsSinceStartOfDay(recMsecTimestamp).toString("hh:mm:ss"));
        }

        if (!m_newMessages.isEmpty())
            addLogMessage(m_newMessages.dequeue());

        QApplication::processEvents();
    }

    // if we are here, we stopped running
    // get last messages
    while (!m_newMessages.isEmpty())
        addLogMessage(m_newMessages.dequeue());

    // reset UI elements
    ui->btnStartStop->setText("Connect");
    ui->btnStartStop->setChecked(false);

    ui->containerScopeControls->setEnabled(false);
    ui->groupBoxDisplay->setEnabled(false);
    ui->btnRecord->setEnabled(false);
    ui->btnStartStop->setEnabled(true);
    ui->labelCurrentFPS->setText(QStringLiteral("???"));
    ui->sbCamId->setEnabled(true);

    if (!m_mscope->lastError().empty())
        QMessageBox::critical(this,
                              "Error",
                              QString::fromStdString(m_mscope->lastError()));
}

void MainWindow::on_sbGain_valueChanged(int arg1)
{
    m_mscope->setGain(arg1);
}

void MainWindow::on_btnRecord_toggled(bool checked)
{
    // don't do anything if miniscope isn't running
    if (!m_mscope->running())
        return;

    QFileInfo dataLocation(dataDir);
    if (!dataLocation.isDir() || !dataLocation.isWritable() || dataDir.isEmpty()) {
        QMessageBox::critical(this,
                              "Recording Error",
                              QStringLiteral("Data location '%1' is not a directory or not writable.").arg(dataDir));
        return;
    }

    if (checked) {
        auto videoFname = QDir(dataDir).filePath(QDateTime::currentDateTime().toString("yy-MM-dd-hhmm") + "_scope").toStdString();
        if (m_mscope->startRecording(videoFname)) {
            ui->gbRecording->setEnabled(false);
            ui->btnStartStop->setEnabled(false);
            ui->btnRecord->setText("Recording...");
        } else {
            ui->btnRecord->setChecked(false);
        }

    } else {
        m_mscope->stopRecording();
        ui->gbRecording->setEnabled(true);
        ui->btnStartStop->setEnabled(true);
        ui->btnRecord->setText("Record");
    }
}

void MainWindow::on_codecComboBox_currentIndexChanged(const QString &arg1)
{
    // reset state of lossless infobox
    ui->losslessCheckBox->setEnabled(true);
    ui->losslessLabel->setEnabled(true);
    ui->losslessCheckBox->setChecked(m_mscope->recordLossless());
    ui->containerComboBox->setEnabled(true);

    if (arg1 == "AV1") {
        m_mscope->setVideoCodec(VideoCodec::AV1);

    } else if (arg1 == "FFV1") {
        m_mscope->setVideoCodec(VideoCodec::FFV1);

        // FFV1 is always lossless
        ui->losslessCheckBox->setEnabled(false);
        ui->losslessLabel->setEnabled(false);
        ui->losslessCheckBox->setChecked(true);

    } else if (arg1 == "VP9") {
        m_mscope->setVideoCodec(VideoCodec::VP9);

    } else if (arg1 == "H.265") {
        m_mscope->setVideoCodec(VideoCodec::H265);

        // H.256 only works with MKV and MP4 containers, select MKV by default
        ui->containerComboBox->setCurrentIndex(0);
        ui->containerComboBox->setEnabled(false);

    } else if (arg1 == "MPEG-4") {
        m_mscope->setVideoCodec(VideoCodec::MPEG4);

        // MPEG-4 can't do lossless encoding
        ui->losslessCheckBox->setEnabled(false);
        ui->losslessLabel->setEnabled(false);
        ui->losslessCheckBox->setChecked(false);

    } else if (arg1 == "None") {
        m_mscope->setVideoCodec(VideoCodec::Raw);

        // Raw is always lossless
        ui->losslessCheckBox->setEnabled(false);
        ui->losslessLabel->setEnabled(false);
        ui->losslessCheckBox->setChecked(true);

        // Raw RGB only works with AVI containers
        ui->containerComboBox->setCurrentIndex(1);
        ui->containerComboBox->setEnabled(false);

    } else
        qCritical() << "Unknown video codec option selected:" << arg1;
}

void MainWindow::on_containerComboBox_currentIndexChanged(const QString &arg1)
{
    if (arg1 == "MKV")
        m_mscope->setVideoContainer(VideoContainer::Matroska);
    else if (arg1 == "AVI")
        m_mscope->setVideoContainer(VideoContainer::AVI);
    else
        qCritical() << "Unknown video container option selected:" << arg1;
}

void MainWindow::on_losslessCheckBox_toggled(bool checked)
{
    m_mscope->setRecordLossless(checked);
}

void MainWindow::on_cbExtRecTrigger_toggled(bool checked)
{
    ui->btnRecord->setChecked(!checked);
    ui->btnRecord->setEnabled(!checked);
    m_mscope->setExternalRecordTrigger(checked);
}

void MainWindow::on_sbDisplayMin_valueChanged(int arg1)
{
    m_mscope->setMinFluorDisplay(arg1);
}


void MainWindow::on_sbDisplayMax_valueChanged(int arg1)
{
    m_mscope->setMaxFluorDisplay(arg1);
}

void MainWindow::on_fpsSpinBox_valueChanged(int arg1)
{
    m_mscope->setFps(static_cast<uint>(arg1));
}

void MainWindow::on_btnOpenSaveDir_clicked()
{
    on_actionSet_Data_Location_triggered();
}

void MainWindow::on_actionSet_Data_Location_triggered()
{
    auto dir = QFileDialog::getExistingDirectory(this,
                                                 "Select Directory",
                                                 QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
                                                 QFileDialog::ShowDirsOnly);
    if (!dir.isEmpty())
        setDataExportDir(dir);
}

void MainWindow::on_actionQuit_triggered()
{
    this->close();
}

void MainWindow::on_actionAbout_Video_Formats_triggered()
{
    const auto infoText = QStringLiteral(
                "<html>"
                "<h3>Which video codec/container should I use?</h3>"
                "<p>PoMiDAQ allows the selction of a few different containers and codecs to store recorded videos. "
                "This brief information may help you decide which format is best suited for your application.</p>"
                "<h4>Matroska (MKV) Container</h4>"
                "<p>This is the most flexible container format. It is fully open-source and patent-free and suitable for long-term storage of "
                "videos. However, some tools such as MATLAB do not natively support it, so if you use MKV you may need to use 3rd-party toolboxes.</p>"
                "<h4>Audio Video Interleave (AVI) Container</h4>"
                "<p>AVI is an old and less flexible container format, which lacks a few features such as standardized ways to store timestamps and aspect ratios. "
                "Due to its age it is very well supported in many tools and may be your first choice if you are aiming for maximum compatibility.</p>"
                "<h4>FFV1 Codec</h4>"
                "<p>This lossless codec is designed for archivability of data and relatively good compression while preserving all information that was present in "
                "the uncompressed image. It is used by many institutions and broadcasting companies and widely supported. Yet, a few tools (such as MATLAB again) may "
                "not natively support it, so you may need to use 3rd-party tools to read the generated data.</p>"
                "<h4>No Codec</h4>"
                "<p>No compression is used to store the images. This will yield very large files, but reading the generated data is relatively easy and supported by many tools.</p>"
                "<h4>Any Other Selectable Codec</h4>"
                "<p>The AV1 codec may become very useful in future, as it is high-quality and open-source and patent-free and an industry standard. However, it is currently too slow "
                "for real-time data acquisition. The same applies to the VP9 codec, unless you record with lower framerates.</p>"
                "<p>H.265 is a popular codec for video compression. It is widely supported and already has fast encoders, but is patent encumbered. You may decide to use it if you need "
                "better compression than FFV1 can offer you and you can read the generated movies.</p>"
                "<p>MPEG-4 is an older video compression standard. You pretty much never want to use it (except for testing), as it is inferior to the other supported codecs.</p>"
                );

    QMessageBox dialog(this);
    dialog.setWindowTitle(QStringLiteral("Video format help"));
    dialog.setInformativeText(infoText);
    dialog.setStandardButtons(QMessageBox::Ok);
    dialog.exec();
}

void MainWindow::on_actionAbout_triggered()
{
    const auto text = QStringLiteral(
                "PoMiDAQ Version " PROJECT_VERSION " \n\n"
                "(c) 2019 Matthias Klumpp\n\n"
                "PoMiDAQ is free software: you can redistribute it and/or modify "
                "it under the terms of the GNU Lesser General Public License as published by "
                "the Free Software Foundation, either version 3 of the License, or "
                "(at your option) any later version.\n"
                "\n"
                "PoMiDAQ is distributed in the hope that it will be useful, "
                "but WITHOUT ANY WARRANTY; without even the implied warranty of "
                "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the "
                "GNU Lesser General Public License for more details.");
    QMessageBox::about(this, QStringLiteral("About this tool"), text);
}

void MainWindow::on_bgSubstCheckBox_toggled(bool checked)
{
    if (checked) {
        ui->bgDivCheckBox->setChecked(false);
        m_mscope->setDisplayBgDiffMethod(BackgroundDiffMethod::SUBTRACTION);
    } else {
        m_mscope->setDisplayBgDiffMethod(BackgroundDiffMethod::NONE);
    }
}

void MainWindow::on_bgDivCheckBox_toggled(bool checked)
{
    if (checked) {
        ui->bgSubstCheckBox->setChecked(false);
        m_mscope->setDisplayBgDiffMethod(BackgroundDiffMethod::DIVISION);
    } else {
        m_mscope->setDisplayBgDiffMethod(BackgroundDiffMethod::NONE);
    }
}

void MainWindow::on_sliceIntervalSpinBox_valueChanged(int arg1)
{
    m_mscope->setRecordingSliceInterval(static_cast<uint>(arg1));
}

void MainWindow::on_accAlphaSpinBox_valueChanged(double arg1)
{
    m_mscope->setBgAccumulateAlpha(arg1);
}

void MainWindow::on_actionShowMiniscopeLog_toggled(bool arg1)
{
    ui->logTextList->setVisible(arg1);
}
