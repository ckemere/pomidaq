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

#include "miniscope.h"

#include <chrono>
#include <thread>
#include <mutex>
#include <boost/circular_buffer.hpp>
#include <boost/format.hpp>

#include "definitions.h"
#include "videowriter.h"

#pragma GCC diagnostic ignored "-Wpadded"
class MiniScopeData
{
public:
    MiniScopeData()
        : thread(nullptr),
          scopeCamId(0),
          connected(false),
          running(false),
          recording(false),
          failed(false)
    {
        frameRing = boost::circular_buffer<cv::Mat>(64);
        videoCodec = VideoCodec::VP9;
        videoContainer = VideoContainer::Matroska;
    }

    std::thread *thread;
    std::mutex mutex;

    cv::VideoCapture cam;
    int scopeCamId;

    int exposure;
    int gain;
    int excitation;
    uint fps;
    bool excitationX10;

    double minFluor;
    double maxFluor;
    int minFluorDisplay;
    int maxFluorDisplay;

    bool connected;
    bool running;
    bool recording;
    bool failed;

    std::chrono::time_point<std::chrono::system_clock> recordStart;

    size_t droppedFramesCount;
    uint currentFPS;

    boost::circular_buffer<cv::Mat> frameRing;

    std::function<void (std::string)> onMessageCallback;

    bool colorCheck;

    VideoCodec videoCodec;
    VideoContainer videoContainer;
    bool recordLossless;
};
#pragma GCC diagnostic pop


MiniScope::MiniScope()
    : d(new MiniScopeData())
{
    d->exposure = 100;
    d->gain = 32;
    d->excitation = 1;
    d->excitationX10 = false;
    d->fps = 20;
}

MiniScope::~MiniScope()
{
    finishCaptureThread();
    setExcitation(0);
    disconnect();
}

void MiniScope::startCaptureThread()
{
    finishCaptureThread();
    d->running = true;
    d->thread = new std::thread(captureThread, this);
}

void MiniScope::finishCaptureThread()
{
    if (d->thread != nullptr) {
        d->running = false;
        d->thread->join();
        delete d->thread;
        d->thread = nullptr;
    }
}

void MiniScope::emitMessage(const std::string &msg)
{
    if (!d->onMessageCallback) {
        std::cout << msg << std::endl;
        return;
    }

    d->mutex.lock();
    d->onMessageCallback(msg);
    d->mutex.unlock();
}

void MiniScope::fail(const std::string &msg)
{
    d->recording = false;
    d->running = false;
    d->failed = true;
    emitMessage(msg);
}

void MiniScope::setScopeCamId(int id)
{
    d->scopeCamId = id;
}

void MiniScope::setExposure(int value)
{
    if (value == 0)
        value = 1;
    if (value > 100)
        value = 100;

    d->exposure = value;
    d->cam.set(CV_CAP_PROP_BRIGHTNESS, static_cast<double>(d->exposure) / 100);
}

int MiniScope::exposure() const
{
    return d->exposure;
}

void MiniScope::setGain(int value)
{
    d->gain = value;
    d->cam.set(CV_CAP_PROP_GAIN, static_cast<double>(d->gain) / 100);
}

int MiniScope::gain() const
{
    return d->gain;
}

void MiniScope::setExcitation(int value)
{
    d->excitation = value;
    setLed(value);
}

int MiniScope::excitation() const
{
    return d->excitation;
}

bool MiniScope::connect()
{
    if (d->connected) {
        std::cerr << "Tried to reconnect already connected camera." << std::endl;
        return false;
    }

    d->cam.open(d->scopeCamId);

    d->cam.set(CV_CAP_PROP_SATURATION, SET_CMOS_SETTINGS); // Initiallizes CMOS sensor (FPS, gain and exposure enabled...)

    // set default values
    setExposure(100);
    setGain(32);
    setExcitation(1);

    d->connected = true;

    //mValueExcitation = 0;
    //mSliderExcitation.SetPos(mValueExcitation);

    setLed(0);

    emitMessage(boost::str(boost::format("Initialized camera %1%") % d->scopeCamId));

    return true;
}

void MiniScope::disconnect()
{
    stop();
    d->cam.release();
    emitMessage(boost::str(boost::format("Disconnected camera %1%") % d->scopeCamId));
}

bool MiniScope::run()
{
    if (!d->connected)
        return false;
    if (d->failed) {
        // try to recover from failed state by reconnecting
        emitMessage("Reconnecting to recover from previous failure.");
        disconnect();
        if (!connect())
            return false;
    }

    startCaptureThread();
    return true;
}

void MiniScope::stop()
{
    d->running = false;
    d->recording = false;
    finishCaptureThread();
}

bool MiniScope::startRecording()
{
    if (!d->connected)
        return false;
    if (!d->running) {
        if (!run())
            return false;
    }

    d->recordStart = std::chrono::high_resolution_clock::now();
    d->recording = true;

    return true;
}

void MiniScope::stopRecording()
{
    d->recording = false;
}

bool MiniScope::running() const
{
    return d->running;
}

bool MiniScope::recording() const
{
    return d->running && d->recording;
}

void MiniScope::setOnMessage(std::function<void(const std::string&)> callback)
{
    d->onMessageCallback = callback;
}

cv::Mat MiniScope::currentFrame()
{
    std::lock_guard<std::mutex> lock(d->mutex);
    cv::Mat frame;
    if (d->frameRing.size() == 0)
        return frame;

    frame = d->frameRing.front();
    d->frameRing.pop_front();
    return frame;
}

uint MiniScope::currentFPS() const
{
    return d->currentFPS;
}

VideoCodec MiniScope::videoCodec() const
{
    return d->videoCodec;
}

void MiniScope::setVideoCodec(VideoCodec codec)
{
    d->videoCodec = codec;
}

VideoContainer MiniScope::videoContainer() const
{
    return d->videoContainer;
}

void MiniScope::setVideoContainer(VideoContainer container)
{
    d->videoContainer = container;
}

bool MiniScope::recordLossless() const
{
    return d->recordLossless;
}

void MiniScope::setRecordLossless(bool lossless)
{
    d->recordLossless = lossless;
}

void MiniScope::setLed(int value)
{
    // sanitize value
    if (value > 100)
        value = 100;

    // maximum brighness reached at 50% already, so we divide by two to allow smaller stepsize
    double ledPower = static_cast<double>(value) / 2 / 100;
    if (d->connected) {
        d->cam.set(CV_CAP_PROP_HUE, ledPower);
    }
}

void MiniScope::addFrameToBuffer(const cv::Mat &frame)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    d->frameRing.push_back(frame);
}

void MiniScope::captureThread(void* msPtr)
{
    MiniScope *self = static_cast<MiniScope*> (msPtr);

    auto currentTime = self->d->recordStart;
    auto previousTime = currentTime;

    std::vector<int> compression_params;
    compression_params.push_back(CV_IMWRITE_PNG_COMPRESSION);
    compression_params.push_back(0);

    cv::Mat droppedFrameImage(cv::Size(752, 480), CV_8UC3);
    droppedFrameImage.setTo(cv::Scalar(255, 0, 0));
    cv::putText(droppedFrameImage,
                "Frame Dropped!",
                cv::Point(24, 240),
                cv::FONT_HERSHEY_COMPLEX,
                1.5,
                cv::Scalar(255,255,255));

    self->d->droppedFramesCount = 0;

    cv::Mat frame;
    // grab initial frame
    auto ret = self->d->cam.grab();
    if (!ret) {
        self->fail("Failed to grab initial frame.");
        return;
    }
    try {
        ret = self->d->cam.retrieve(frame);
    } catch (cv::Exception& e) {
        ret = false;
        auto msg = boost::str(boost::format("Caught OpenCV exception: %1%") % e.what());
        std::cerr << msg << std::endl;
        self->emitMessage(msg);
    }
    if (!ret) {
        self->fail("Failed to retrieve initial frame.");
        return;
    }

    std::unique_ptr<VideoWriter> vwriter(new VideoWriter());
    vwriter->setCodec(self->d->videoCodec);
    vwriter->setContainer(self->d->videoContainer);
    vwriter->setLossless(self->d->recordLossless);
    vwriter->initialize("/tmp/testvideo",
                        frame.cols,
                        frame.rows,
                        static_cast<int>(self->d->fps),
                        frame.channels() == 3);

    while (self->d->running) {


#if 0
        //Added for triggerable recording
        if (self->mCheckTrigRec == true) {
            temp = self->msCam.get(CV_CAP_PROP_SATURATION);
            //str.Format(L"GPIO State: %u",temp);
            //self->AddListText(str);
            if ((temp & TRIG_RECORD_EXT) == TRIG_RECORD_EXT) {
                if (self->record == false) {
                    self->setLed(0,self->mValueExcitation);
                    self->OnBnClickedRecord();//Start recording
                }
            }
            else {
                if(self->record == true) {
                    self->OnBnClickedStoprecord();
                    self->setLed(0,0);

                }
            }
        }
#endif

        auto status = self->d->cam.grab();
        if (!status) {
            self->fail("Failed to grab frame.");
            continue;
        }

        previousTime = currentTime;
        currentTime = std::chrono::high_resolution_clock::now();
        try {
            status = self->d->cam.retrieve(frame);
        } catch (cv::Exception& e) {
            status = false;
            std::cerr << "Caught OpenCV exception:" << e.what() << std::endl;
        }

        if (!status) {
            //self->record = false;
            self->d->droppedFramesCount++;
            self->emitMessage("Dropped frame.");
            self->addFrameToBuffer(droppedFrameImage);
            if (self->d->droppedFramesCount > 0) {
                self->emitMessage("Reconnecting Miniscope...");
                self->d->cam.release();
                self->d->cam.open(self->d->scopeCamId);
                self->emitMessage("Miniscope reconnected.");
            }

            if (self->d->droppedFramesCount > 80)
                self->fail("Too many dropped frames. Giving up.");
            continue;
        }

            /*
            if (self->d->droppedFramesCount > 0 || self->d->currentFPS < self->d->fps / 2.0) {
                std::cout << "Sending settings." << std::endl;
                self->d->cam.set(CV_CAP_PROP_BRIGHTNESS, self->d->exposure);
                self->d->cam.set(CV_CAP_PROP_GAIN, self->d->gain);
                self->setLed(0, self->d->excitation);
                self->d->droppedFramesCount = 0;
                continue;
            }
            */

#if 0
            if (self->getScreenShot == true) {

                CT2CA pszConvertedAnsiString = self->folderLocation + "\\" + self->mMouseName + "_" + self->mNote + "_" + self->currentTime + ".png";
                tempString = pszConvertedAnsiString;
                cv::imwrite(tempString,self->msFrame[self->msWritePos%BUFFERLENGTH],compression_params);
                self->getScreenShot = false;
                str.Format(L"Image saved in %s",self->folderLocation);
                self->AddListText(str);
                self->mNote = "";
                self->UpdateData(FALSE);
            }
            //cv::cvtColor(self->msFrame[self->msWritePos%BUFFERLENGTH],frame,CV_YUV2GRAY_YUYV);//added to correct green color stream
#endif

            if (self->d->colorCheck) {
                //cv::Mat frame;

                //cv::Mat channel[3];
                cv::cvtColor(frame, frame, CV_BGR2GRAY);
                //cv::cvtColor(self->msFrame[self->msWritePos%BUFFERLENGTH],frame,CV_YUV2GRAY_YUY2);//added to correct green color stream
                cv::cvtColor(frame, frame,CV_BayerRG2BGR);

#if 0
                if (self->mRed == TRUE || self->mGreen == TRUE) {
                    cv::Mat bgrChannels[3];
                    cv::split(frame,bgrChannels);
                    bgrChannels[0] = cv::Mat::zeros(frame.rows,frame.cols,CV_8UC1);
                    if (self->mRed == FALSE)
                        bgrChannels[2] = cv::Mat::zeros(frame.rows,frame.cols,CV_8UC1);
                    if (self->mGreen == FALSE)
                        bgrChannels[1] = cv::Mat::zeros(frame.rows,frame.cols,CV_8UC1);
                    cv::merge(bgrChannels,3,frame);
                }
#endif


                self->addFrameToBuffer(frame);
                vwriter->encodeFrame(frame);
            } else {

                cv::cvtColor(frame, frame, CV_BGR2GRAY); // added to correct green color stream

                cv::minMaxLoc(frame, &self->d->minFluor, &self->d->maxFluor);
                frame.convertTo(frame, CV_8U, 255.0 / (self->d->maxFluorDisplay - self->d->minFluorDisplay), -self->d->minFluorDisplay * 255.0 / (self->d->maxFluorDisplay - self->d->minFluorDisplay));

              //!  if (self->record == true)
                    self->addFrameToBuffer(frame);

                    //vwriter->encodeFrame(frame);
                   // cv::imshow("msCam",frame);//added to correct green color stream
               //! else {
               //     cv::Mat dst;
                    //cv::threshold(self->msFrame[self->msWritePos%BUFFERLENGTH],dst,self->mSaturationThresh,0,4);
                    //cv::threshold(frame,dst,self->mSaturationThresh,0,4);//added to correct green color stream
                    //cv::imshow("msCam", dst);
              //!      cv::imshow("msCam",frame);

              //!  }

            }

#if 0
            if (self->record == true) {
                msSingleLock.Lock();  // Attempt to lock the shared resource
                if (msSingleLock.IsLocked()) { // Resource has been locked
                    self->msWritePos++;
                    msSingleLock.Unlock();
                }
            }
#endif

            std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    vwriter->finalize();
}