﻿#include <opencv2/opencv.hpp>

#define _USE_MATH_DEFINES
#include <math.h>

#include "MarkerTracker.hpp"
#include "PoseEstimation.h"

using namespace cv;
using namespace std;


void trackbarHandler(int pos, void* slider_value) {
	*((int*)slider_value) = pos;
}

void bw_trackbarHandler(int pos, void* slider_value) {
	*((int*)slider_value) = pos;
}


int subpixSampleSafe(const cv::Mat &pSrc, const cv::Point2f &p)
{
	int x = int(floorf(p.x));
	int y = int(floorf(p.y));

	if (x < 0 || x >= pSrc.cols - 1 ||
		y < 0 || y >= pSrc.rows - 1)
		return 127;

	int dx = int(256 * (p.x - floorf(p.x)));
	int dy = int(256 * (p.y - floorf(p.y)));

	unsigned char* i = (unsigned char*)((pSrc.data + y * pSrc.step) + x);
	int a = i[0] + ((dx * (i[1] - i[0])) >> 8);
	i += pSrc.step;
	int b = i[0] + ((dx * (i[1] - i[0])) >> 8);
	return a + ((dy * (b - a)) >> 8);
}

void MarkerTracker::init()
{
	std::cout << "Startup\n";
	cv::namedWindow(kWinName1, CV_WINDOW_AUTOSIZE);
	cv::namedWindow(kWinName2, CV_WINDOW_NORMAL);
	cv::namedWindow(kWinName4, CV_WINDOW_NORMAL);
	cv::namedWindow(kWinName5, CV_WINDOW_NORMAL);

	cv::resizeWindow(kWinName4, 120, 120);

	int max = 255;
	int slider_value = 100;
	cv::createTrackbar("Threshold", kWinName2, &slider_value, 255, trackbarHandler, &slider_value);

	int bw_sileder_value = bw_thresh;
	cv::createTrackbar("BW Threshold", kWinName2, &slider_value, 255, bw_trackbarHandler, &bw_sileder_value);

	memStorage = cvCreateMemStorage();
}

void MarkerTracker::cleanup()
{
	cvReleaseMemStorage(&memStorage);

	cv::destroyWindow(kWinName1);
	cv::destroyWindow(kWinName2);
	cv::destroyWindow(kWinName4);
	cv::destroyWindow(kWinName5);

	std::cout << "Finished\n";
}

bool lineIntersection(Point2f o1, Point2f p1, Point2f o2, Point2f p2,
	Point2f &r)
{
	Point2f x = o2 - o1;
	Point2f d1 = p1 - o1;
	Point2f d2 = p2 - o2;

	float cross = d1.x*d2.y - d1.y*d2.x;
	if (abs(cross) < /*EPS*/1e-8)
		return false;

	double t1 = (x.x * d2.y - x.y * d2.x) / cross;
	r = o1 + d1 * t1;
	return true;
}

void findPreciseCornerPoints(Point2f cornerPoints[4], Mat lineParamsMat) {
	//At this point we have four lines saved in the rows of lineParamsMat
	//We intersect these to get the four corners
	for (int i = 0; i<4; i++) {

		Point2f line1point1(lineParamsMat.at<float>(2, i), lineParamsMat.at<float>(3, i));
		Point2f line1point2(lineParamsMat.at<float>(2, i) + 30 * lineParamsMat.at<float>(0, i), lineParamsMat.at<float>(3, i) + 30 * lineParamsMat.at<float>(1, i));

		Point2f line2point1(lineParamsMat.at<float>(2, (i + 1) % 4), lineParamsMat.at<float>(3, (i + 1) % 4));
		Point2f line2point2(lineParamsMat.at<float>(2, (i + 1) % 4) + 30 * lineParamsMat.at<float>(0, (i + 1) % 4), lineParamsMat.at<float>(3, (i + 1) % 4) + 30 * lineParamsMat.at<float>(1, (i + 1) % 4));

		Point2f lineIntersect;

		lineIntersection(line1point1, line1point2, line2point1, line2point2, lineIntersect);
		cornerPoints[i] = lineIntersect;
	}
}

cv::Point2f corners[4];
cv::Point2f targetCorners[4];

cv::Point2f MarkerTracker::relativeToAbsolute(cv::Point2f relative_point) {
	
	// Projektive Transformationsmatrix generieren, mit der das OpenCV Spielfeld auf das virtuelle Feld transformiert wird
	cv::Mat warpMatrix = cv::Mat(cv::Size(3, 3), CV_32FC1);
	warpMatrix = cv::getPerspectiveTransform(corners, targetCorners);

	// Wir verwenden den Typ Matx33f - mit dem Typ können wir einfach eine Matrix-Vektor-Multiplikation machen.
	// Geht mit dem normalen Mat-Datentyp nicht.
	// mit Hilfe von http://stackoverflow.com/questions/17852182/runnig-cvwarpperspective-on-points heraus gefunden
	// Im Nachhinein: Man könnte stattdessen auch unseren "relative_point" in eine Mat verwandeln (cv::Mat(relative_point, false)) -> dann würde Multiplikation gehen. 
	cv::Matx33f warp = warpMatrix;

	// Punkt relative zum Spielfeld (Koordinaten des Punkts entsprechen Koordinaten relativ zum Spielfeld)

	// Punkt mit der inversen projektiven Transformationsmatrix multiplizieren
	cv::Point3f homogeneos = warp.inv() * relative_point;

	// Ergebnispunkt -> nun relativ zum gesamten Kamerabild
	cv::Point2f result(homogeneos.x, homogeneos.y);

	//std::cout << "x: " << result.x << ", y: " << result.y << "\n";
	return result;
}



void MarkerTracker::findMarker(cv::Mat &img_bgr, float resultMatrix[16])
{
	bool isFirstStripe = true;

	bool isFirstMarker = true;


	{
		cv::cvtColor(img_bgr, img_gray, CV_BGR2GRAY);
		cv::threshold(img_gray, img_mono, thresh, 255, CV_THRESH_BINARY);

		// Find Contours with old OpenCV APIs
		CvSeq* contours;
		CvMat img_mono_(img_mono);

		cvFindContours(
			&img_mono_, memStorage, &contours, sizeof(CvContour),
			CV_RETR_LIST, CV_CHAIN_APPROX_SIMPLE
		);

		for (; contours; contours = contours->h_next)
		{
			CvSeq* result = cvApproxPoly(
				contours, sizeof(CvContour), memStorage, CV_POLY_APPROX_DP,
				cvContourPerimeter(contours)*0.02, 0
			);

			if (result->total != 4) {
				continue;
			}

			cv::Mat result_ = cv::cvarrToMat(result); /// API 1.X to 2.x
			cv::Rect r = cv::boundingRect(result_);
			if (r.height<40 || r.width<40 || r.height>150 || r.width>150 || r.width > img_mono_.cols - 10 || r.height > img_mono_.rows - 10) {
				continue;
			}

			const cv::Point *rect = (const cv::Point*) result_.data;
			int npts = result_.rows;
			// draw the polygon
			cv::polylines(img_bgr, &rect, &npts, 1,
				true,           // draw closed contour (i.e. joint end to start)
				CV_RGB(255, 0, 0),// colour RGB ordering (here = green)
				2,              // line thickness
				CV_AA, 0);


			float lineParams[16];
			cv::Mat lineParamsMat(cv::Size(4, 4), CV_32F, lineParams); // lineParams is shared

			for (int i = 0; i<4; ++i)
			{
				cv::circle(img_bgr, rect[i], 3, CV_RGB(0, 255, 0), -1);

				double dx = (double)(rect[(i + 1) % 4].x - rect[i].x) / 7.0;
				double dy = (double)(rect[(i + 1) % 4].y - rect[i].y) / 7.0;

				int stripeLength = (int)(0.8*sqrt(dx*dx + dy*dy));
				if (stripeLength < 5)
					stripeLength = 5;

				//make stripeLength odd (because of the shift in nStop)
				stripeLength |= 1;

				//e.g. stripeLength = 5 --> from -2 to 2
				int nStop = stripeLength >> 1;
				int nStart = -nStop;

				cv::Size stripeSize;
				stripeSize.width = 3;
				stripeSize.height = stripeLength;

				cv::Point2f stripeVecX;
				cv::Point2f stripeVecY;

				//normalize vectors
				double diffLength = sqrt(dx*dx + dy*dy);
				stripeVecX.x = dx / diffLength;
				stripeVecX.y = dy / diffLength;

				stripeVecY.x = stripeVecX.y;
				stripeVecY.y = -stripeVecX.x;

				cv::Mat iplStripe(stripeSize, CV_8UC1);
				///				IplImage* iplStripe = cvCreateImage( stripeSize, IPL_DEPTH_8U, 1 );

				// Array for edge point centers
				cv::Point2f points[6];

				for (int j = 1; j<7; ++j)
				{
					double px = (double)rect[i].x + (double)j*dx;
					double py = (double)rect[i].y + (double)j*dy;

					cv::Point p;
					p.x = (int)px;
					p.y = (int)py;
					cv::circle(img_bgr, p, 2, CV_RGB(0, 0, 255), -1);

					for (int m = -1; m <= 1; ++m)
					{
						for (int n = nStart; n <= nStop; ++n)
						{
							cv::Point2f subPixel;

							subPixel.x = (double)p.x + ((double)m * stripeVecX.x) + ((double)n * stripeVecY.x);
							subPixel.y = (double)p.y + ((double)m * stripeVecX.y) + ((double)n * stripeVecY.y);

							cv::Point p2;
							p2.x = (int)subPixel.x;
							p2.y = (int)subPixel.y;

							if (isFirstStripe)
								cv::circle(img_bgr, p2, 1, CV_RGB(255, 0, 255), -1);
							else
								cv::circle(img_bgr, p2, 1, CV_RGB(0, 255, 255), -1);

							int pixel = subpixSampleSafe(img_gray, subPixel);

							int w = m + 1; //add 1 to shift to 0..2
							int h = n + (stripeLength >> 1); //add stripelenght>>1 to shift to 0..stripeLength


							iplStripe.at<uchar>(h, w) = (uchar)pixel;
							///							*(iplStripe->imageData + h * iplStripe->widthStep  + w) =  pixel; //set pointer to correct position and safe subpixel intensity
						}
					}

					//use sobel operator on stripe
					// ( -1 , -2, -1 )
					// (  0 ,  0,  0 )
					// (  1 ,  2,  1 )
					std::vector<double> sobelValues(stripeLength - 2);
					///					double* sobelValues = new double[stripeLength-2];
					for (int n = 1; n < (stripeLength - 1); n++)
					{
						unsigned char* stripePtr = &(iplStripe.at<uchar>(n - 1, 0));
						///						unsigned char* stripePtr = ( unsigned char* )( iplStripe->imageData + (n-1) * iplStripe->widthStep );
						double r1 = -stripePtr[0] - 2 * stripePtr[1] - stripePtr[2];

						stripePtr += 2 * iplStripe.step;
						///						stripePtr += 2*iplStripe->widthStep;
						double r3 = stripePtr[0] + 2 * stripePtr[1] + stripePtr[2];
						sobelValues[n - 1] = r1 + r3;
					}

					double maxVal = -1;
					int maxIndex = 0;
					for (int n = 0; n<stripeLength - 2; ++n)
					{
						if (sobelValues[n] > maxVal)
						{
							maxVal = sobelValues[n];
							maxIndex = n;
						}
					}

					double y0, y1, y2; // y0 .. y1 .. y2
					y0 = (maxIndex <= 0) ? 0 : sobelValues[maxIndex - 1];
					y1 = sobelValues[maxIndex];
					y2 = (maxIndex >= stripeLength - 3) ? 0 : sobelValues[maxIndex + 1];

					//formula for calculating the x-coordinate of the vertex of a parabola, given 3 points with equal distances
					//(xv means the x value of the vertex, d the distance between the points):
					//xv = x1 + (d / 2) * (y2 - y0)/(2*y1 - y0 - y2)

					double pos = (y2 - y0) / (4 * y1 - 2 * y0 - 2 * y2); //d = 1 because of the normalization and x1 will be added later

																		 // This would be a valid check, too
																		 //if (std::isinf(pos)) {
																		 //	// value is infinity
																		 //	continue;
																		 //}

					if (pos != pos) {
						// value is not a number
						continue;
					}

					cv::Point2f edgeCenter; //exact point with subpixel accuracy
					int maxIndexShift = maxIndex - (stripeLength >> 1);

					//shift the original edgepoint accordingly
					edgeCenter.x = (double)p.x + (((double)maxIndexShift + pos) * stripeVecY.x);
					edgeCenter.y = (double)p.y + (((double)maxIndexShift + pos) * stripeVecY.y);

					cv::Point p_tmp;
					p_tmp.x = (int)edgeCenter.x;
					p_tmp.y = (int)edgeCenter.y;
					cv::circle(img_bgr, p_tmp, 1, CV_RGB(0, 0, 255), -1);

					points[j - 1].x = edgeCenter.x;
					points[j - 1].y = edgeCenter.y;

					if (isFirstStripe)
					{
						cv::Mat iplTmp;
						cv::resize(iplStripe, iplTmp, cv::Size(100, 300));

						isFirstStripe = false;
					}

				} // end of loop over edge points of one edge

				  // we now have the array of exact edge centers stored in "points"
				cv::Mat mat(cv::Size(1, 6), CV_32FC2, points);
				///TODO:transponiere lineParamsMat und nehme dann col oder row
				cv::fitLine(mat, lineParamsMat.col(i), CV_DIST_L2, 0, 0.01, 0.01);


			} // end of loop over the 4 edges

			  // so far we stored the exact line parameters and show the lines in the image
			  // now we have to calculate the exact corners
			

			findPreciseCornerPoints(corners, lineParamsMat);




			targetCorners[0].x = -0.5; targetCorners[0].y = -0.5;
			targetCorners[1].x = 5.5; targetCorners[1].y = -0.5;
			targetCorners[2].x = 5.5; targetCorners[2].y = 5.5;
			targetCorners[3].x = -0.5; targetCorners[3].y = 5.5;

			//create and calculate the matrix of perspective transform
			cv::Mat projMat(cv::Size(3, 3), CV_32FC1);
			projMat = cv::getPerspectiveTransform(corners, targetCorners);
			///			cv::warpPerspectiveQMatrix ( corners, targetCorners, projMat);


			//create image for the marker
			//			markerSize.width  = 6;
			//			markerSize.height = 6;
			cv::Mat iplMarker(cv::Size(6, 6), CV_8UC1);

			//change the perspective in the marker image using the previously calculated matrix
			cv::warpPerspective(img_gray, iplMarker, projMat, cv::Size(6, 6));
			cv::threshold(iplMarker, iplMarker, bw_thresh, 255, CV_THRESH_BINARY);
			cv::imshow(kWinName4, iplMarker);

			//Grid 2D
			cv::Mat grid(cv::Size(200, 400), CV_8UC1);
			cv::warpPerspective(img_bgr, grid, projMat, cv::Size(200, 400));

			cv::imshow(kWinName5, grid);



			// check if border is black
			int code = 0;
			for (int i = 0; i < 6; ++i)
			{
				//int pixel1 = ((unsigned char*)(iplMarker->imageData + 0*iplMarker->widthStep + i))[0]; //top
				//int pixel2 = ((unsigned char*)(iplMarker->imageData + 5*iplMarker->widthStep + i))[0]; //bottom
				//int pixel3 = ((unsigned char*)(iplMarker->imageData + i*iplMarker->widthStep))[0]; //left
				//int pixel4 = ((unsigned char*)(iplMarker->imageData + i*iplMarker->widthStep + 5))[0]; //right
				int pixel1 = iplMarker.at<uchar>(0, i);
				int pixel2 = iplMarker.at<uchar>(5, i);
				int pixel3 = iplMarker.at<uchar>(i, 0);
				int pixel4 = iplMarker.at<uchar>(i, 5);
				if ((pixel1 > 0) || (pixel2 > 0) || (pixel3 > 0) || (pixel4 > 0))
				{
					code = -1;
					break;
				}
			}

			if (code < 0) {
				continue;
			}

			//copy the BW values into cP
			int cP[4][4];
			for (int i = 0; i < 4; ++i)
			{
				for (int j = 0; j < 4; ++j)
				{
					cP[i][j] = iplMarker.at<uchar>(i + 1, j + 1);
					cP[i][j] = (cP[i][j] == 0) ? 1 : 0; //if black then 1 else 0
				}
			}

			//save the ID of the marker
			int codes[4];
			codes[0] = codes[1] = codes[2] = codes[3] = 0;
			for (int i = 0; i < 16; i++)
			{
				int row = i >> 2;
				int col = i % 4;

				codes[0] <<= 1;
				codes[0] |= cP[row][col]; // 0�

				codes[1] <<= 1;
				codes[1] |= cP[3 - col][row]; // 90�

				codes[2] <<= 1;
				codes[2] |= cP[3 - row][3 - col]; // 180�

				codes[3] <<= 1;
				codes[3] |= cP[col][3 - row]; // 270�
			}

			if ((codes[0] == 0) || (codes[0] == 0xffff)) {
				continue;
			}

			//account for symmetry
			code = codes[0];
			int angle = 0;
			for (int i = 1; i<4; ++i)
			{
				if (codes[i] < code)
				{
					code = codes[i];
					angle = i;
				}
			}

//			printf("Found: %04x\n", code);

			if (isFirstMarker)
			{
				isFirstMarker = false;
			}

			//correct the order of the corners
			if (angle != 0)
			{
				cv::Point2f corrected_corners[4];
				for (int i = 0; i < 4; i++)	corrected_corners[(i + angle) % 4] = corners[i];
				for (int i = 0; i < 4; i++)	corners[i] = corrected_corners[i];
			}

			// transfer screen coords to camera coords
			for (int i = 0; i < 4; i++)
			{
				corners[i].x -= img_bgr.cols*0.5; //here you have to use your own camera resolution (x) * 0.5
				corners[i].y = -corners[i].y + img_bgr.rows*0.5; //here you have to use your own camera resolution (y) * 0.5
			}


			estimateSquarePose(resultMatrix, (cv::Point2f*)corners, kMarkerSize);

			//this part is only for printing
//			for (int i = 0; i<4; ++i) {
//				for (int j = 0; j<4; ++j) {
//					std::cout << resultMatrix[4 * i + j] << " ";
//				}
//				std::cout << "\n";
			//}
//			std::cout << "\n";
			float x, y, z;
			x = resultMatrix[3];
			y = resultMatrix[7];
			z = resultMatrix[11];
//			std::cout << "length: " << sqrt(x*x + y*y + z*z) << "\n";
//			std::cout << "\n";
		} // end of loop over contours

		cv::imshow(kWinName1, img_bgr);
		cv::imshow(kWinName2, img_mono);
		
		



		isFirstStripe = true;

		isFirstMarker = true;


		cvClearMemStorage(memStorage);
	} // end of main loop

	cvClearMemStorage(memStorage);

	int key = cvWaitKey(10);
	if (key == 27) exit(0);

	//	glutPostRedisplay();
}