/******************************************************************************
 *                                                                            *
 * Copyright (C) 2020 Fondazione Istituto Italiano di Tecnologia (IIT)        *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <limits>
#include <fstream>

#include <yarp/os/Network.h>
#include <yarp/os/LogStream.h>
#include <yarp/os/ResourceFinder.h>
#include <yarp/os/RFModule.h>
#include <yarp/os/Value.h>
#include <yarp/os/Bottle.h>
#include <yarp/os/Property.h>
#include <yarp/os/RpcServer.h>
#include <yarp/os/RpcClient.h>
#include <yarp/os/Time.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/dev/PolyDriver.h>
#include <yarp/dev/ControlBoardInterfaces.h>
#include <yarp/dev/CartesianControl.h>
#include <yarp/dev/GazeControl.h>
#include <yarp/sig/Vector.h>
#include <yarp/sig/Matrix.h>
#include <yarp/sig/Image.h>
#include <yarp/sig/PointCloud.h>
#include <yarp/math/Math.h>

#include "rpc_IDL.h"
#include "segmentation.h"
#include "viewer.h"

using namespace std;
using namespace yarp::os;
using namespace yarp::dev;
using namespace yarp::sig;
using namespace yarp::math;
using namespace segmentation;
using namespace viewer;

/******************************************************************************/
class Grasper : public RFModule, public rpc_IDL {
    PolyDriver arm_r,arm_l;
    PolyDriver hand_r,hand_l;
    PolyDriver gaze;

    RpcServer rpcPort;
    BufferedPort<ImageOf<PixelRgb>> rgbPort;
    BufferedPort<ImageOf<PixelFloat>> depthPort;
    RpcClient sqPort;

    vector<int> fingers = {7, 8, 9, 10, 11, 12, 13, 14, 15};
    shared_ptr<yarp::sig::PointCloud<DataXYZRGBA>> pc_scene;
    shared_ptr<yarp::sig::PointCloud<DataXYZRGBA>> pc_table;
    shared_ptr<yarp::sig::PointCloud<DataXYZRGBA>> pc_object;

    Matrix Teye;
    double view_angle{50.};
    double table_height{numeric_limits<double>::quiet_NaN()};
    Bottle sqParams;
    
    unique_ptr<Viewer> viewer;
    
    /**************************************************************************/
    bool attach(RpcServer& source) override {
        return this->yarp().attachAsServer(source);
    }

    /**************************************************************************/
    bool savePCL(const string& filename, shared_ptr<yarp::sig::PointCloud<DataXYZRGBA>> pc) {
        ofstream fout(filename);
        if (fout.is_open()) {
            fout << "COFF" << endl;
            fout << pc->size() << " 0 0" << endl;
            for (size_t i = 0; i < pc->size(); i++) {
                const auto& p = (*pc)(i);
                fout << p.x << " " << p.y << " " << p.z << " "
                     << (int)p.r << " " << (int)p.g << " "
                     << (int)p.b << " " << (int)p.a << endl;
            }
            return true;
        } else {
            yError() << "Unable to write to" << filename;
            return false;
        }
    }

    /**************************************************************************/
    bool helperArmDriverOpener(PolyDriver& arm, const Property& options) {
        const auto t0 = Time::now();
        while (Time::now() - t0 < 10.) {
            // this might fail if controller is not connected to solver yet
            if (arm.open(const_cast<Property&>(options))) {
                return true;
            }
            Time::delay(1.);
        }
        return false;
    }

    /**************************************************************************/
    bool configure(ResourceFinder& rf) override {
        string name = "icub-grasp";

        Property arm_r_options;
        arm_r_options.put("device", "cartesiancontrollerclient");
        arm_r_options.put("local", "/"+name+"/arm_r");
        arm_r_options.put("remote", "/icubSim/cartesianController/right_arm");
        if (!helperArmDriverOpener(arm_r, arm_r_options)) {
            yError() << "Unable to open right arm driver!";
            return false;
        }

        Property arm_l_options;
        arm_l_options.put("device", "cartesiancontrollerclient");
        arm_l_options.put("local", "/"+name+"/arm_l");
        arm_l_options.put("remote", "/icubSim/cartesianController/left_arm");
        if (!helperArmDriverOpener(arm_l, arm_l_options)) {
            yError() << "Unable to open left arm driver!";
            arm_r.close();
            return false;
        }

        Property hand_r_options;
        hand_r_options.put("device", "remote_controlboard");
        hand_r_options.put("local", "/"+name+"/hand_r");
        hand_r_options.put("remote", "/icubSim/right_arm");
        if (!hand_r.open(hand_r_options)) {
            yError() << "Unable to open right hand driver!";
            arm_r.close();
            arm_l.close();
            return false;
        }

        Property hand_l_options;
        hand_l_options.put("device", "remote_controlboard");
        hand_l_options.put("local", "/"+name+"/hand_l");
        hand_l_options.put("remote", "/icubSim/left_arm");
        if (!hand_l.open(hand_l_options)) {
            yError() << "Unable to open left hand driver!";
            arm_r.close();
            arm_l.close();
            hand_r.close();
            return false;
        }

        Property gaze_options;
        gaze_options.put("device", "gazecontrollerclient");
        gaze_options.put("local", "/"+name+"/gaze");
        gaze_options.put("remote", "/iKinGazeCtrl");
        if (!gaze.open(gaze_options)) {
            yError() << "Unable to open gaze driver!";
            arm_r.close();
            arm_l.close();
            hand_r.close();
            hand_l.close();
            return false;
        }

        // set up velocity of arms' movements
        {
            vector<PolyDriver*> polys({&arm_r, &arm_l});
            for (auto poly:polys) {
                ICartesianControl* iarm;
                poly->view(iarm);
                iarm->setTrajTime(.6);
            }
        }

        // enable position control of the fingers
        {
            vector<PolyDriver*> polys({&hand_r, &hand_l});
            for (auto poly:polys) {
                IControlMode* imod;
                poly->view(imod);
                imod->setControlModes(fingers.size(), fingers.data(), vector<int>(fingers.size(), VOCAB_CM_POSITION).data());
            }
        }

        rgbPort.open("/"+name+"/rgb:i");
        depthPort.open("/"+name+"/depth:i");
        sqPort.open("/"+name+"/sq:rpc");
        rpcPort.open("/"+name+"/rpc");
        attach(rpcPort);

        viewer = unique_ptr<Viewer>(new Viewer(10, 370, 350, 350));
        viewer->start();

        return true;
    }

    /**************************************************************************/
    bool go() override {
        if (home()) {
            Time::delay(2.);
            if (segment()) {
                if (fit()) {
                    if (grasp()) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /**************************************************************************/
    bool home() override {
        // home gazing
        IGazeControl* igaze;
        gaze.view(igaze);
        igaze->lookAtFixationPoint(Vector({-.3, 0., 0.}));

        // home arms
        {
            Vector x({-.25, .3, .1});
            vector<PolyDriver*> polys({&arm_r, &arm_l});
            ICartesianControl* iarm;
            for (auto poly:polys) {                
                poly->view(iarm);
                iarm->goToPositionSync(x);                
                x[1] = -x[1];
            }
            // wait only for the last arm
            iarm->waitMotionDone(.1, 3.);
        }

        return true;
    }

    /**************************************************************************/
    bool segment() override {
        // get camera extrinsic matrix
        IGazeControl* igaze;
        gaze.view(igaze);
        Vector cam_x, cam_o;
        igaze->getLeftEyePose(cam_x, cam_o);
        Teye = axis2dcm(cam_o);
        Teye.setSubcol(cam_x, 0, 3);

        // get image data
        ImageOf<PixelRgb>* rgbImage = rgbPort.read();
        ImageOf<PixelFloat>* depthImage = depthPort.read();

        if ((rgbImage == nullptr) || (depthImage == nullptr)) {
            yError() << "Unable to receive image data!";
            return false;
        }

        if ((rgbImage->width() != depthImage->width()) ||
            (rgbImage->height() != depthImage->height()) ) {
            yError() << "Received image data with wrong size!";
            return false;
        }

        const auto w = rgbImage->width();
        const auto h = rgbImage->height();

        // aggregate image data in the point cloud of the whole scene
        pc_scene = shared_ptr<yarp::sig::PointCloud<DataXYZRGBA>>(new yarp::sig::PointCloud<DataXYZRGBA>);
        const auto fov_h = (w / 2.) / tan((view_angle / 2.) * (M_PI / 180.));
        Vector x({0., 0., 0., 1.});
        for (int v = 0; v < h; v++) {
            for (int u = 0; u < w; u++) {
                const auto rgb = (*rgbImage)(u, v);
                const auto depth = (*depthImage)(u, v);
                
                if (depth > 0.F) {
                    x[0] = depth * (u - .5 * (w - 1)) / fov_h;
                    x[1] = depth * (v - .5 * (h - 1)) / fov_h;
                    x[2] = depth;
                    x = Teye * x;
                
                    pc_scene->push_back(DataXYZRGBA());
                    auto& p = (*pc_scene)(pc_scene->size() - 1);
                    p.x = (float)x[0];
                    p.y = (float)x[1];
                    p.z = (float)x[2];
                    p.r = rgb.r;
                    p.g = rgb.g;
                    p.b = rgb.b;
                    p.a = 255;
                }
            }
        }

        savePCL("/workspace/pc_scene.off", pc_scene);

        // segment out the table and the object
        pc_table = shared_ptr<yarp::sig::PointCloud<DataXYZRGBA>>(new yarp::sig::PointCloud<DataXYZRGBA>);
        pc_object = shared_ptr<yarp::sig::PointCloud<DataXYZRGBA>>(new yarp::sig::PointCloud<DataXYZRGBA>);
        table_height = Segmentation::RANSAC(pc_scene, pc_table, pc_object);
        if (isnan(table_height)) {
            yError() << "Segmentation failed!";
            return false;
        }

        savePCL("/workspace/pc_table.off", pc_table);
        savePCL("/workspace/pc_object.off", pc_object);

        // update viewer
        Vector cam_foc;
        igaze->get3DPointOnPlane(0, {w/2., h/2.}, {0., 0., 1., -table_height}, cam_foc);
        viewer->addCamera({cam_x[0], cam_x[1], cam_x[2]}, {cam_foc[0], cam_foc[1], cam_foc[2]}, {0., 0., 1.}, view_angle);
        viewer->addTable({cam_foc[0], cam_foc[1], cam_foc[2]}, {0., 0., 1.});
        viewer->addObject(pc_object);

        return true;
    }

    /**************************************************************************/
    bool fit() override {
        if (sqPort.getOutputCount() > 0) {
            // offload object fitting to the external solver
            const auto ret = sqPort.write(*pc_object, sqParams);
            if (ret) {
                viewer->addSuperquadric(sqParams);
                return true;
            } else {
                yError() << "Unable to fit the object!";
                return false;
            }
        } else {
            yError() << "Not connected to the solver";
            return false;
        }
    }

    /**************************************************************************/
    bool grasp() override {
        // target pose that allows grasing the object
        Vector x({-.24, .18, -.03});
        Vector o({-.14, -.79, .59, 3.07});

        // keep gazing at the object
        IGazeControl* igaze;
        gaze.view(igaze);
        igaze->setTrackingMode(true);
        igaze->lookAtFixationPoint(x);

        // reach for the pre-grasp pose
        ICartesianControl* iarm;
        arm_r.view(iarm);
        Vector dof({1, 0, 1, 1, 1, 1, 1, 1, 1, 1});
        iarm->setDOF(dof, dof);
        iarm->goToPoseSync(x + Vector({.07, 0., .03}), o);
        
        // put the hand in the pre-grasp configuration
        IPositionControl* ihand;
        IControlLimits* ilim;
        hand_r.view(ihand);
        hand_r.view(ilim);
        double pinkie_min, pinkie_max;
        ilim->getLimits(15, &pinkie_min, &pinkie_max);
        ihand->setRefAccelerations(fingers.size(), fingers.data(), vector<double>(fingers.size(), numeric_limits<double>::infinity()).data());
        ihand->setRefSpeeds(fingers.size(), fingers.data(), vector<double>({60., 60., 60., 60., 60., 60., 60., 60., 200.}).data());
        ihand->positionMove(fingers.size(), fingers.data(), vector<double>({60., 80., 0., 0., 0., 0., 0., 0., pinkie_max}).data());
        Time::delay(5.);

        // make sure that we've reached for the pre-grasp pose
        iarm->waitMotionDone(.1, 3.);
        
        // increase reaching accuracy
        Bottle options;
        Bottle& opt1 = options.addList();
        opt1.addString("tol"); opt1.addDouble(.001);
        Bottle& opt2 = options.addList();
        opt2.addString("constr_tol"); opt2.addDouble(.000001);
        iarm->tweakSet(options);
        iarm->setInTargetTol(.001);

        // reach for the object
        iarm->goToPoseSync(x, o);
        iarm->waitMotionDone(.1, 5.);

        // close fingers
        ihand->positionMove(fingers.size(), fingers.data(), vector<double>({60., 80., 40., 35., 40., 35., 40., 35., pinkie_max}).data());

        // give time to adjust the contacts
        Time::delay(5.);

        // lift up the object
        x += Vector({-.07, 0., .1});
        igaze->lookAtFixationPoint(x);
        iarm->goToPoseSync(x, o);
        iarm->waitMotionDone(.1, 3.);

        return true;
    }

    /**************************************************************************/
    double getPeriod() override {
        return 1.0;
    }

    /**************************************************************************/
    bool updateModule() override {
        return true;
    }

    bool interruptModule() override {
        viewer->stop();

        // interrupt blocking read
        sqPort.interrupt();
        depthPort.interrupt();
        rgbPort.interrupt();
        return true;
    }

    /**************************************************************************/
    bool close() override {
        // restore default contexts
        IGazeControl* igaze;
        gaze.view(igaze);
        igaze->restoreContext(0);

        vector<PolyDriver*> polys({&arm_r, &arm_l});
        for (auto poly:polys) {
            ICartesianControl* iarm;
            poly->view(iarm);
            iarm->restoreContext(0);
        }

        rpcPort.close();
        sqPort.close();
        depthPort.close();
        rgbPort.close();
        gaze.close();
        arm_r.close();
        arm_l.close();
        hand_r.close();
        hand_l.close();
        return true;
    }
};

/******************************************************************************/
int main(int argc, char *argv[]) {
    Network yarp;
    if (!yarp.checkNetwork()) {
        yError() << "Unable to find YARP server!";
        return EXIT_FAILURE;
    }

    ResourceFinder rf;
    rf.configure(argc,argv);

    Grasper grasper;
    return grasper.runModule(rf);
}