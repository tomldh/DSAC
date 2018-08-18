/*
Copyright (c) 2016, TU Dresden
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the TU Dresden nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL TU DRESDEN BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>

#include "properties.h"
#include "thread_rand.h"
#include "util.h"
#include "stop_watch.h"
#include "dataset.h"

#include "lua_calls.h"
#include "cnn.h"

#include <fstream>

int main(int argc, const char* argv[])
{
    // read in parameters
    GlobalProperties* gp = GlobalProperties::getInstance();
    gp->parseConfig();
    gp->parseCmdLine(argc, argv);

    int objHyps = gp->pP.ransacIterations;;

    int ptCount = 3;
    int inlierThreshold2D = gp->pP.ransacInlierThreshold2D;
    int refInlierCount = gp->pP.ransacBatchSize;
    int refSteps = gp->pP.ransacRefinementIterations;
    
    std::string baseScriptRGB = gp->dP.objScript;
    std::string baseScriptObj = gp->dP.scoreScript;
    std::string modelFileRGB = gp->dP.objModel;
    std::string modelFileObj = gp->dP.scoreModel;
   
    // load test data
    std::string dataDir = "./test/";
    
    std::vector<std::string> testSets = getSubPaths(dataDir);
    std::cout << std::endl << BLUETEXT("Loading test set ...") << std::endl;
    jp::Dataset testDataset = jp::Dataset(testSets[0], 1);

    //check if test set is  empty
    if(!testDataset.size())
    {
        std::cout << std::endl << REDTEXT("The test set is empty!") << std::endl;
        return 0;
    }

/*
	// Export registered depth image
	for (unsigned int idx = 0; idx < testDataset.size(); ++idx)
	{
//     std::cout << "Reading File" << trainDataset.depthFiles[idx] << std::endl;
	   jp::img_bgrd_t imgBGRD;
	   testDataset.getBGRD(idx, imgBGRD);

	   const std::string dFile = testDataset.depthFiles[idx].substr(testDataset.depthFiles[idx].find_last_of('/')+1);
//     std::cout << "Processing depth: " << dFile << std::endl;

	   cv::imwrite(dFile, imgBGRD.depth);
	}
	exit(0);
*/
        
    // lua and models
    std::cout << "Loading script: " << baseScriptObj << std::endl;    
    lua_State* stateObj = luaL_newstate();
    luaL_openlibs(stateObj);
    execute(baseScriptObj.c_str(), stateObj);
    loadModel(modelFileObj, CNN_OBJ_CHANNELS, CNN_OBJ_PATCHSIZE, stateObj);    
    setEvaluate(stateObj);
    
    std::cout << "Loading script: " << baseScriptRGB << std::endl;    
    lua_State* stateRGB = luaL_newstate();
    luaL_openlibs(stateRGB);
    execute(baseScriptRGB.c_str(), stateRGB);    
    loadModel(modelFileRGB, CNN_RGB_CHANNELS, CNN_RGB_PATCHSIZE, stateRGB);
    setEvaluate(stateRGB);
       
    cv::Mat camMat = gp->getCamMat();
    
    std::ofstream testFile;
    testFile.open("ransac_test_loss_"+modelFileRGB+"_rdraw"+intToString(gp->pP.randomDraw)+".txt"); // contains evaluation information for the whole test sequence
    
    double avgCorrect = 0;
    
    std::vector<double> expLosses;
    std::vector<double> sfEntropies;
    std::vector<double> rotErrs;
    std::vector<double> tErrs;
    
    std::ofstream testErrFile;
    testErrFile.open("ransac_test_errors_"+modelFileRGB+"_rdraw"+intToString(gp->pP.randomDraw)+".txt"); // contains evaluation information for each test image
	
    for(unsigned i = 0; i < testDataset.size(); i++)
    {
        std::cout << YELLOWTEXT("Processing test image " << i << " of " << testDataset.size()) << "." << std::endl;

        // load test image
        jp::img_bgr_t testRGB;
        testDataset.getBGR(i, testRGB);

        jp::info_t testInfo;
        testDataset.getInfo(i, testInfo);

        // process frame (same function used in training, hence most of the variables below are not used here), see method documentation for parameter explanation
        std::vector<jp::cv_trans_t> hyps;
        std::vector<jp::cv_trans_t> refHyps;
        std::vector<std::vector<cv::Point3f>> imgdPts;
        std::vector<std::vector<cv::Point3f>> objPts;
        std::vector<std::vector<int>> imgIdx;
        std::vector<cv::Mat_<cv::Vec3f>> patches;
        std::vector<double> sfScores;
        jp::img_coord_t estObj;
        cv::Mat_<cv::Point2i> sampling;
        std::vector<std::vector<cv::Point2i>> sampledPoints;
        std::vector<double> losses;
        std::vector<cv::Mat_<int>> inlierMaps;
        std::vector<std::vector<std::vector<int>>> pixelIdxs; //hyps -> refIts -> pixelIdxs
        double tErr;
        double rotErr;
        int hypIdx;

        jp::img_coord_t camPts;
        jp::img_coord_t camPtsMap;
        testDataset.getCamPts(i, camPts);

        double expectedLoss;
        double sfEntropy;
        bool correct;

        processImage(
            testRGB,
            Hypothesis(testInfo),
            stateRGB,
            stateObj,
            objHyps,
            ptCount,
            camMat,
            inlierThreshold2D,
            refInlierCount,
            refSteps,
            expectedLoss,
            sfEntropy,
            correct,
            hyps,
            refHyps,
            imgdPts,
            objPts,
            imgIdx,
            patches,
            sfScores,
            estObj,
			camPts,
			camPtsMap,
            sampling,
            sampledPoints,
            losses,
            inlierMaps,
            pixelIdxs,
            tErr,
            rotErr,
            hypIdx);

        avgCorrect += correct;

        // convert back to 7-Scenes norm
        jp::jp_trans_t jpHyp = jp::cv2our(refHyps[hypIdx]);
        Hypothesis hyp(jpHyp.first, jpHyp.second);

        cv::Point3d hypT = hyp.getTranslation();
        cv::Mat hypR = hyp.getRotation();
        cv::Mat_<double> hypTrans = cv::Mat_<double>::eye(4, 4);
        hypR.copyTo(hypTrans.colRange(0, 3).rowRange(0, 3));
        hypTrans(0, 3) = hypT.x;
        hypTrans(1, 3) = hypT.y;
        hypTrans(2, 3) = hypT.z;

        hypTrans = hypTrans.inv();

        cv::Mat_<double> correction = cv::Mat_<double>::eye(4, 4);
        correction.col(1) = -correction.col(1);
        correction.col(2) = -correction.col(2);
        hypTrans = hypTrans * correction;

        hypT.x = hypTrans(0, 3);
        hypT.y = hypTrans(1, 3);
        hypT.z = hypTrans(2, 3);
        hyp.setTranslation(hypT);

        hypTrans.colRange(0, 3).rowRange(0, 3).copyTo(hypR);
        hyp.setRotation(hypR);

        // transform estimated pose to Rodriguez vector + translation
        std::vector<double> hypV = hyp.getRodVecAndTrans();

        // translation im m
        hypV[3] /= 1000;
        hypV[4] /= 1000;
        hypV[5] /= 1000;

        // correct optional translation
        std::ifstream transFile("translation.txt");

        std::string line;
        std::vector<std::string> tokens;

        if(transFile.is_open())
        {
            std::getline(transFile, line);
            tokens = split(line);
            hypV[3] += std::atof(tokens[0].c_str());
            hypV[4] += std::atof(tokens[1].c_str());
            hypV[5] += std::atof(tokens[2].c_str());
            transFile.close();
        }

        testErrFile
            << expectedLoss << " "      // 0  - expected loss over the hypothesis pool
            << sfEntropy << " "         // 1  - entropy of the hypothesis score distribution
            << losses[hypIdx] << " "    // 2  - loss of the selected hypothesis
            << tErr << " "              // 3  - translational error in mm
            << rotErr << " "            // 4  - rotational error in deg
            << hypV[0] << " "           // 5  - selected pose, rotation (1st component of Rodriguez vector)
            << hypV[1] << " "           // 6  - selected pose, rotation (2nd component of Rodriguez vector)
            << hypV[2] << " "           // 7  - selected pose, rotation (3th component of Rodriguez vector)
            << hypV[3] << " "           // 8  - selected pose, translation in m (x)
            << hypV[4] << " "           // 9  - selected pose, translation in m (y)
            << hypV[5] << " "           // 10 - selected pose, translation in m (z)
            << std::endl;

        // store statistics for calculation of mean, median, stddev
        expLosses.push_back(expectedLoss);
        sfEntropies.push_back(sfEntropy);
        tErrs.push_back(tErr);
        rotErrs.push_back(rotErr);
    }
    
    // mean and stddev of loss
    std::vector<double> lossMean;
    std::vector<double> lossStdDev;
    cv::meanStdDev(expLosses, lossMean, lossStdDev);
    
    // mean and stddev of score distribution entropy
    std::vector<double> entropyMean;
    std::vector<double> entropyStdDev;
    cv::meanStdDev(sfEntropies, entropyMean, entropyStdDev);	  	
	
    avgCorrect /= testDataset.size();
    
    // median of rotational and translational errors
    std::sort(rotErrs.begin(), rotErrs.end());
    std::sort(tErrs.begin(), tErrs.end());
    
    double medianRotErr = rotErrs[rotErrs.size() / 2];
    double medianTErr = tErrs[tErrs.size() / 2];
    
    std::cout << "-----------------------------------------------------------" << std::endl;
    std::cout << BLUETEXT("Avg. test loss: " << lossMean[0] << ", accuracy: " << avgCorrect * 100 << "%") << std::endl;
    std::cout << "Median Rot. Error: " << medianRotErr << "deg, Median T. Error: " << medianTErr / 10 << "cm." << std::endl;

    testFile
        << avgCorrect << " "            // 0 - percentage of correct poses
        << lossMean[0] << " "           // 1 - mean loss of selected hypotheses
        << lossStdDev[0] << " "         // 2 - standard deviation of losses of selected hypotheses
        << entropyMean[0] << " "        // 3 - mean of the score distribution entropy
        << entropyStdDev[0] << " "      // 4 - standard deviation of the score distribution entropy
        << medianRotErr << " "          // 5 - median rotational error of selected hypotheses
        << medianTErr                   // 6 - median translational error (in mm) of selected hypotheses
        << std::endl;
    
    testFile.close();
    testErrFile.close();
    
    lua_close(stateRGB);
    lua_close(stateObj);
    
    return 0;    
}
