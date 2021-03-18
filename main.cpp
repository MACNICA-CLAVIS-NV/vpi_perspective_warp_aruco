/*
MIT License
Copyright (c) 2021 MACNICA Inc.
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/*
 *	A perspective warp demonstration program using NVIDIA VPI
 */


#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/aruco.hpp>

#include <vpi/OpenCVInterop.hpp>
#include <vpi/Context.h>
#include <vpi/Image.h>
#include <vpi/Status.h>
#include <vpi/Stream.h>
#include <vpi/algo/ConvertImageFormat.h>
#include <vpi/algo/PerspectiveWarp.h>
#include <vpi/algo/Rescale.h>

#include <iostream>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>

#define RET_OK	0
#define RET_ERR	-1

extern char* optarg;

bool LoopFlag = true;

typedef struct {
	char	*videoFile;
	int		cameraId;
	int		capWidth;
	int		capHeight;
} AppArgs;

#define DEBUG_PRINT(str) \
	std::cout << __FILE__ << "(" << __LINE__ << "): " << str << std::endl


#define CHECK_STATUS(STMT)									  \
	do														  \
	{														  \
		VPIStatus status = (STMT);							  \
		if (status != VPI_SUCCESS)							  \
		{													  \
			char buffer[VPI_MAX_STATUS_MESSAGE_LENGTH]; 	  \
			vpiGetLastStatusMessage(buffer, sizeof(buffer));  \
			std::ostringstream ss;							  \
			ss << vpiStatusGetName(status) << ": " << buffer; \
			throw std::runtime_error(ss.str()); 			  \
		}													  \
	} while (0);


void signalHandler(int sig)
{
	std::cout << "Aborted." << std::endl;
	LoopFlag = false;
}


// Utility function to wrap a cv::Mat into a VPIImage
static VPIImage ToVPIImage(VPIImage image, const cv::Mat &frame)
{
    if (image == nullptr)
    {
        // Ceate a VPIImage that wraps the frame
        CHECK_STATUS(vpiImageCreateOpenCVMatWrapper(frame, 0, &image));
    }
    else
    {
        // reuse existing VPIImage wrapper to wrap the new frame.
        CHECK_STATUS(vpiImageSetWrappedOpenCVMat(image, frame));
    }
    return image;
}


static int parseArgs(int argc, char *argv[], AppArgs *pArgs)
{
	option options[] = {
		{"video",  required_argument, 0, 'v'},
		{"camera", required_argument, 0, 'c'},
		{"width",  required_argument, 0, 'w'},
		{"height", required_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	int err = RET_OK;
	int optIdx = 0;
	int c, ret;

	/* Set the default values */
	pArgs->videoFile = nullptr;
	pArgs->cameraId = 0;
	pArgs->capWidth = 640;
	pArgs->capHeight = 480;

	while(1) {
		c = getopt_long (argc, argv, "v:c:w:h:", options, &optIdx);
		if (c < 0) {
			break;
		}

		ret = 0;

		switch (c) {
		case 'v':
			pArgs->videoFile = (char *)malloc(sizeof(char) * (strlen(optarg) + 1));
			strcpy(pArgs->videoFile, optarg);
			break;
		case 'c':
			ret = sscanf(optarg, "%d", &(pArgs->cameraId));
			break;
		case 'w':
			ret = sscanf(optarg, "%d", &(pArgs->capWidth));
			break;
		case 'h':
			ret = sscanf(optarg, "%d", &(pArgs->capHeight));
			break;
		default:
			err = RET_ERR;
			break;
		}

		if (ret == EOF) {
			err = RET_ERR;
		}

		if (err != RET_OK) {
			break;
		}
	}

	if (pArgs->videoFile == nullptr) {
		printf("Video file not specified\n");
		err = RET_ERR;
	}

	return (err);
}


static void releaseArgs(AppArgs *pArgs)
{
	if (pArgs->videoFile != nullptr) {
		free(pArgs->videoFile);
		pArgs->videoFile = nullptr;
	}
}


static int openCamera(AppArgs& args, cv::VideoCapture& cap)
{
	cap.open(args.cameraId, cv::CAP_V4L2);
	if (!cap.isOpened()) {
		throw std::runtime_error("Unable to open camera: " + args.cameraId);
	}
	std::cout << "Backend API: " << cap.getBackendName() << std::endl;
	
	cap.set(cv::CAP_PROP_FRAME_WIDTH, (double)args.capWidth);
	cap.set(cv::CAP_PROP_FRAME_HEIGHT, (double)args.capHeight);
	cap.set(cv::CAP_PROP_FPS, 30.0);
	cap.set(cv::CAP_PROP_BUFFERSIZE, (double)3);
	
	/* Get the actual configurations */
	args.capWidth = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
	args.capHeight = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
	int fps = cap.get(cv::CAP_PROP_FPS);
	int bufSize = (int)cap.get(cv::CAP_PROP_BUFFERSIZE);
	std::cout << "Frame width :" << args.capWidth << std::endl;
	std::cout << "Frame height:" << args.capHeight << std::endl;
	std::cout << "FPS         :" << fps << std::endl;
	std::cout << "Buffer Size :" << bufSize << std::endl;

	return (RET_OK);
}


int getOutPoints(std::vector<std::vector<cv::Point2f>>& corners,
		std::vector<int>& ids, cv::Point2f *ptDst)
{
	int ret = RET_OK;

	for (int id = 0;id < 4;id++) {
		int idx = -1;
		for (int i = 0;i < 4;i++) {
			if (ids[i] == id) {
				idx = i;
				break;
			}
		}
		if (idx < 0) {
			std::cerr << "id not found" << std::endl;
			ret = RET_ERR;
			break;
		}

		ptDst[id] = cv::Point(corners[idx][id].x, corners[idx][id].y);
	}

	return (ret);
}


int main(int argc, char *argv[])
{
	int ret = RET_OK;

	AppArgs args;
	int err = parseArgs(argc, argv, &args);
	if (err) {
		releaseArgs(&args);
		ret = RET_ERR;
		return (ret);
	}

	cv::VideoCapture cap;
	VPIContext ctx = nullptr;

	try {
		/* Open the camera */
		openCamera(args, cap);

		/* Open the video file to be warpped */
		cv::VideoCapture invid;
		if (!invid.open(args.videoFile)) {
			std::string msg = "Can't open the video file: ";
			msg.append(args.videoFile);
			throw std::runtime_error(msg);
		}
		int vidWidth = (int)invid.get(cv::CAP_PROP_FRAME_WIDTH);
		int vidHeight = (int)invid.get(cv::CAP_PROP_FRAME_HEIGHT);
		std::cout << vidWidth << ", " << vidHeight << std::endl;
	
		cv::Mat frame, frameCopy, warpFrame; 

		cv::Mat image(
			args.capHeight, args.capWidth, CV_8UC3, cv::Scalar(0, 0, 255)
		);

		/* Create the context */
		CHECK_STATUS(
			vpiContextCreate(0, &ctx)
		);

		/* Activate it. From now on all created objects will be owned by it. */
		CHECK_STATUS(
			vpiContextSetCurrent(ctx)
		);

		/* Create the stream for the given backend */
		VPIStream stream;
		CHECK_STATUS(
			vpiStreamCreate(VPI_BACKEND_CUDA, &stream)
		);

		VPIImage imgInput, imgOutput, imgTemp;
		CHECK_STATUS(
			vpiImageCreate(
				args.capWidth, args.capHeight, 
				VPI_IMAGE_FORMAT_NV12_ER, 0, &imgInput
			)
		);
		CHECK_STATUS(
			vpiImageCreate(
				args.capWidth, args.capHeight, 
				VPI_IMAGE_FORMAT_NV12_ER, 0, &imgOutput
			)
		);
		CHECK_STATUS(
			vpiImageCreate(vidWidth, vidHeight, 
				VPI_IMAGE_FORMAT_NV12_ER, 0, &imgTemp)
		);

		/* Create a Perspective Warp payload */
		VPIPayload warp;
		CHECK_STATUS(
			vpiCreatePerspectiveWarp(VPI_BACKEND_CUDA, &warp)
		);

		VPIPerspectiveTransform xform;
		memset(&xform, 0, sizeof(xform));

		VPIImage imgVid = nullptr;
		VPIImage imgDisp = nullptr;

		cv::Ptr<cv::aruco::DetectorParameters> detectorParams
			= cv::aruco::DetectorParameters::create();
		int dictionaryId = 0;
		cv::Ptr<cv::aruco::Dictionary> dictionary =
			cv::aruco::getPredefinedDictionary(
				cv::aruco::PREDEFINED_DICTIONARY_NAME(dictionaryId));

		std::cout << "Start grabbing" << std::endl
			<< "Press any key to terminate" << std::endl;

		cv::Mat cvFrame;

		if (signal(SIGINT, signalHandler) == SIG_ERR) {
			ret = RET_ERR;
			return (ret);
		}

		while (LoopFlag) {
			/* Capture a frame */
			cap.read(frame);
			if (frame.empty()) {
				std::cerr << "ERROR! blank frame grabbed" << std::endl;
				break;
			}

			std::vector<int> ids;
			std::vector<std::vector<cv::Point2f>> corners, rejected;

			cv::aruco::detectMarkers(
				frame, dictionary, corners, ids, detectorParams, rejected
			);

			frame.copyTo(frameCopy);
			if (ids.size() > 0 && ids.size() < 4) {
				cv::aruco::drawDetectedMarkers(frameCopy, corners, ids);
			}

			cv::Point2f ptSrc[4] = {
				cv::Point2f(0.0, 0.0),
				cv::Point2f((float)args.capWidth, 0.0),
				cv::Point2f((float)args.capWidth, (float)args.capHeight),
				cv::Point2f(0.0, (float)args.capHeight)
			};
			cv::Point2f ptDst[4];
			cv::Point pt[4];
			if (ids.size() == 4) {
				if(getOutPoints(corners, ids, ptDst) != RET_OK) {
					/* Display the captured frame */
					cv::imshow("Capture", frameCopy);
					continue;
				}
				for (int id = 0;id < 4;id++) {
					pt[id] = ptDst[id];
				}

				cv::fillConvexPoly(
					frameCopy, pt, 4, cv::Scalar(0, 0, 0)
				);

				cv::Mat tmtrx = cv::getPerspectiveTransform(ptSrc, ptDst);

				bool ret = invid.read(cvFrame);
				if (!ret) {
					invid.release();
					break;
				}

				imgVid = ToVPIImage(imgVid, cvFrame);
				imgDisp = ToVPIImage(imgDisp, frame);

				// First convert it to NV12 using CUDA
				CHECK_STATUS(
					vpiSubmitConvertImageFormat(
						stream, VPI_BACKEND_CUDA, imgVid, imgTemp, NULL
					)
				);

				// Rescale
				CHECK_STATUS(
					vpiSubmitRescale(
						stream, VPI_BACKEND_CUDA, imgTemp, imgInput, 
						VPI_INTERP_LINEAR, VPI_BORDER_ZERO, 0	
					)
				);

				for (int i = 0;i < 3;i++) {
					for (int j = 0;j < 3;j++) {
							xform[i][j] = tmtrx.at<double>(i, j);
					}
				}

				// Do perspective warp using the backend passed in the command line.
				CHECK_STATUS(
					vpiSubmitPerspectiveWarp(
						stream, 0, warp, imgInput, xform, imgOutput, 
						VPI_INTERP_LINEAR, VPI_BORDER_ZERO, 0
					)
				);

				// Convert output back to BGR using CUDA
				CHECK_STATUS(
					vpiSubmitConvertImageFormat(
						stream, VPI_BACKEND_CUDA, imgOutput, imgDisp, NULL
					)
				);
				CHECK_STATUS(
					vpiStreamSync(stream)
				);

				// Now add it to the output video stream
				VPIImageData imgdata;
				CHECK_STATUS(
					vpiImageLock(imgDisp, VPI_LOCK_READ, &imgdata)
				);
				CHECK_STATUS(
					vpiImageDataExportOpenCVMat(imgdata, &warpFrame)
				);
				CHECK_STATUS(
					vpiImageUnlock(imgDisp)
				);
				cv::add(warpFrame, frameCopy, frameCopy);
				cv::imshow("Capture", frameCopy);
			}
			else {
				/* Display the captured frame */
				cv::imshow("Capture", frameCopy);
			}
		
			/* Check if any key pressed */
			if (cv::waitKey(5) >= 0) {
				break;
			}
		}
	}
	catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
        ret = RET_ERR;
    }

	// Clean up
	vpiContextDestroy(ctx);
	std::cout << "Context destoried." << std::endl;
	
	/* Release the camera device */
	cap.release();
	std::cout << "Video capture released." << std::endl;

	releaseArgs(&args);
	
	return (ret);
}
