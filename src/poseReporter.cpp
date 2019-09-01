
#define camera_delay 0.10
#define armLength 0.2775
#define maxSpeed 2.0
#define maxDistInit 100.0

#define min_frequency 3
#define max_frequency 36.0
#define boundary_ratio 0.5

#define qpix 3
//pixel std. dev


/* #include <std_srvs/Trigger.h> */
#include <experimental/filesystem>
#include <cv_bridge/cv_bridge.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <geometry_msgs/PoseWithCovariance.h>
#include <nav_msgs/Odometry.h>
/* #include <image_transport/image_transport.h> */
#include <mrs_msgs/Vec4.h>
#include <mrs_msgs/TrackerDiagnostics.h>
/* #include <mrs_msgs/TrackerTrajectorySrv.h> */
#include <mrs_msgs/Vec1.h>
/* #include <nav_msgs/Odometry.h> */
#include <ros/package.h>
#include <ros/ros.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/Imu.h>
#include <std_msgs/Float32.h>
#include <std_msgs/MultiArrayDimension.h>
#include <uvdar/Int32MultiArrayStamped.h>
#include <std_msgs/Int32MultiArray.h>
#include <std_srvs/SetBool.h>
#include <stdint.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>
#include <OCamCalib/ocam_functions.h>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <mutex>
#include <numeric>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <thread>
#include "unscented/unscented.h"
/* #include <mrs_lib/mrs_lib/Lkf.h> */

static double sqr(double a){
  return a*a;
}

static double cot(double input) {
  return cos(input) / sin(input);
}

static double deg2rad(double input) {
  return input*0.01745329251;
}

namespace enc = sensor_msgs::image_encodings;

class PoseReporter {
public:
  PoseReporter(ros::NodeHandle& node) {
    ROS_INFO("Initializing pose reporter...");
    reachedTarget   = false;
    followTriggered = false;
    ros::NodeHandle private_node_handle("~");
    private_node_handle.param("uav_name", uav_name, std::string());
    /* ROS_INFO("UAV_NAME: %s",uav_name.c_str()); */

    private_node_handle.param("DEBUG", DEBUG, bool(false));

    private_node_handle.param("gui", gui, bool(false));

    private_node_handle.param("legacy", _legacy, bool(false));
    if (_legacy){
      private_node_handle.param("legacy_delay", _legacy_delay, double(0.2));
      ROS_INFO_STREAM("Legacy mode in effect. Set delay is " << _legacy_delay << "s");
    }

    private_node_handle.param("frequenciesPerTarget", frequenciesPerTarget, int(4));
    private_node_handle.param("targetCount", targetCount, int(4));
    int frequencyCount = targetCount*frequenciesPerTarget;
 
    int tempFreq;
    if (frequencySet.size() < frequencyCount) {
      private_node_handle.param("frequency1", tempFreq, int(6));
      frequencySet.push_back(double(tempFreq));
    }
    if (frequencySet.size() < frequencyCount) {
      private_node_handle.param("frequency2", tempFreq, int(10));
      frequencySet.push_back(double(tempFreq));
    }
    if (frequencySet.size() < frequencyCount) {
      private_node_handle.param("frequency3", tempFreq, int(15));
      frequencySet.push_back(double(tempFreq));
    }
    if (frequencySet.size() < frequencyCount) {
      private_node_handle.param("frequency4", tempFreq, int(30));
      frequencySet.push_back(double(tempFreq));
    }
    if (frequencySet.size() < frequencyCount) {
      private_node_handle.param("frequency5", tempFreq, int(8));
      frequencySet.push_back(double(tempFreq));
    }
    if (frequencySet.size() < frequencyCount) {
      private_node_handle.param("frequency6", tempFreq, int(12));
      frequencySet.push_back(double(tempFreq));
    }

    prepareFrequencyClassifiers();

    char calib_path[400];
    private_node_handle.param("calib_file", _calib_file, std::string("calib_results_bf_uv_fe.txt"));
    sprintf(calib_path, "%s/include/OCamCalib/config/%s", ros::package::getPath("uvdar").c_str(),_calib_file.c_str());
    get_ocam_model(&oc_model, calib_path);

    mask_active = false;
    char mask_path[400];
    private_node_handle.param("mask_file", _mask_file, std::string("dummy"));
    sprintf(mask_path, "%s/masks/%s", ros::package::getPath("uvdar").c_str(),_mask_file.c_str());
    setMask(mask_path);

    /* subscribe to blinkersSeen //{ */

    std::vector<std::string> blinkersSeenTopics;
    private_node_handle.param("blinkersSeenTopics", blinkersSeenTopics, blinkersSeenTopics);
    if (blinkersSeenTopics.empty()) {
      ROS_WARN("[PoseReporter]: No topics of blinkers were supplied");
    }

    // Create callbacks for each camera
    for (size_t i = 0; i < blinkersSeenTopics.size(); ++i) {
      blinkers_seen_callback_t callback = [imageIndex=i,this] (const uvdar::Int32MultiArrayStampedConstPtr& pointsMessage) { 
        ProcessPoints(pointsMessage, imageIndex);
      };
      blinkersSeenCallbacks.push_back(callback);
    // Subscribe to corresponding topics
      ROS_INFO_STREAM("[PoseReporter]: Subscribing to " << blinkersSeenTopics[i]);
      blinkersSeenSubscribers.push_back(
          private_node_handle.subscribe(blinkersSeenTopics[i], 1, &blinkers_seen_callback_t::operator(), &blinkersSeenCallbacks[i]));
    }

    //}
    
    /* subscribe to estimatedFramerate //{ */

    std::vector<std::string> _estimated_framerate_topics;

    private_node_handle.param("estimatedFramerateTopics", _estimated_framerate_topics, _estimated_framerate_topics);
    // fill the framerates with -1
    estimatedFramerate.insert(estimatedFramerate.begin(), _estimated_framerate_topics.size(), -1);

    if (_estimated_framerate_topics.empty()) {
      ROS_WARN("[PoseReporter]: No topics of estimated framerates were supplied");
    }
    if (blinkersSeenTopics.size() != _estimated_framerate_topics.size()) {
      ROS_ERROR_STREAM("[PoseReporter]: The size of blinkersSeenTopics (" << blinkersSeenTopics.size() <<
          ") is different from estimatedFramerateTopics (" << _estimated_framerate_topics.size() << ")");
    }

    // Create callbacks for each camera
    for (size_t i = 0; i < _estimated_framerate_topics.size(); ++i) {
      estimated_framerate_callback_t callback = [imageIndex=i,this] (const std_msgs::Float32ConstPtr& framerateMessage) { 
        estimatedFramerate[imageIndex] = framerateMessage->data;
      };
      estimatedFramerateCallbacks.push_back(callback);
    }
    // Subscribe to corresponding topics
    for (size_t i = 0; i < _estimated_framerate_topics.size(); ++i) {
      ROS_INFO_STREAM("[PoseReporter]: Subscribing to " << _estimated_framerate_topics[i]);
      estimatedFramerateSubscribers.push_back(
          private_node_handle.subscribe(_estimated_framerate_topics[i], 1, &estimated_framerate_callback_t::operator(), &estimatedFramerateCallbacks[i]));
    }

    //}

    measuredPose.resize(targetCount);
    ROS_INFO("[%s]: targetCount: %d", ros::this_node::getName().c_str(), targetCount );
    for (int i=0;i<targetCount;i++){
      ROS_INFO("[%s]: Advertising measuredPose%d", ros::this_node::getName().c_str(), i+1);
      measuredPose[i] = private_node_handle.advertise<geometry_msgs::PoseWithCovarianceStamped>("measuredPose" + std::to_string(i+1), 1);
    }

    X2 = Eigen::VectorXd(9,9);
    X3 = Eigen::VectorXd(10,10);
    Px2 = Eigen::MatrixXd(9,9);
    Px3 = Eigen::MatrixXd(10,10);

    if (_legacy){
      ROS_ERROR("[%s] Legacy is not supported any more", ros::this_node::getName().c_str());
    }

    foundTarget = false;

    /* transformations for cameras //{ */

    private_node_handle.param("cameraFrames", cameraFrames, cameraFrames);
    if (cameraFrames.size() != blinkersSeenTopics.size()) {
      ROS_ERROR_STREAM("The size of cameraFrames (" << cameraFrames.size() << 
          ") is different from blinkersSeenTopics size (" << blinkersSeenTopics.size() << ")");
    }

    tf_thread   = std::thread(&PoseReporter::TfThread, this);

    //}
    
  }

  ~PoseReporter() {
  }

  Eigen::VectorXd uvdarHexarotorPose3p(Eigen::VectorXd X, Eigen::VectorXd expFrequencies){
      cv::Point3d tmp;
      cv::Point3d a(X(0),X(1),X(2));
      cv::Point3d b(X(3),X(4),X(5));
      cv::Point3d c(X(6),X(7),X(8));
      double ambig = X(9);

      if ((c.x) < (a.x)) {
        tmp = a;
        a = c;
        c = tmp;
      }
      if ((b.x) < (a.x)) {
        tmp = b;
        b = a;
        a = tmp;
      }
      if ((b.x) > (c.x)) {
        tmp = c;
        c   = b;
        b   = tmp;
      }
      std::cout << "central led: " << b << std::endl;
      Eigen::Vector3i ids;
      ids << 0,1,2;
      Eigen::Vector3d expPeriods;
      expPeriods = expFrequencies.cwiseInverse(); 
      Eigen::Vector3d periods;
      periods << a.z,b.z,c.z;
      Eigen::Vector3d id;
      Eigen::MatrixXd::Index   minIndex;
      ((expPeriods.array()-(periods(0))).cwiseAbs()).minCoeff(&minIndex);
      id(0) = minIndex;
      ((expPeriods.array()-(periods(1))).cwiseAbs()).minCoeff(&minIndex);
      id(1) = minIndex;
      ((expPeriods.array()-(periods(2))).cwiseAbs()).minCoeff(&minIndex);
      id(2) = minIndex;


      double pixDist = (cv::norm(b - a) + cv::norm(c - b)) * 0.5;
      double v1[3], v2[3], v3[3];
      double va[2] = {(double)(a.y), (double)(a.x)};
      double vb[2] = {(double)(b.y), (double)(b.x)};
      double vc[2] = {(double)(c.y), (double)(c.x)};

      cam2world(v1, va, &oc_model);
      cam2world(v2, vb, &oc_model);
      cam2world(v3, vc, &oc_model);

      Eigen::Vector3d V1(v1[1], v1[0], -v1[2]);
      Eigen::Vector3d V2(v2[1], v2[0], -v2[2]);
      Eigen::Vector3d V3(v3[1], v3[0], -v3[2]);

      Eigen::Vector3d norm13=V3.cross(V1);
      norm13=norm13/norm13.norm();
      double dist132=V2.dot(norm13);
      Eigen::Vector3d V2_c=V2-dist132*norm13;
      V2_c=V2_c/V2_c.norm();

      double Alpha = acos(V1.dot(V2_c));
      double Beta  = acos(V2_c.dot(V3));
  
      double A = 1.0 / tan(Alpha);
      double B = 1.0 / tan(Beta);
      std::cout << "alpha: " << Alpha << " beta: " << Beta << std::endl;
      std::cout << "A: " << A << " B: " << B << std::endl;

      double O = (A * A - A * B + sqrt(3.0) * A + B * B + sqrt(3.0) * B + 3.0);
      /* std::cout << "long operand: " << O << std::endl; */
      double delta = 2.0 * atan(((B * (2.0 * sqrt(O / (B * B + 2.0 * sqrt(3.0) + 3.0)) - 1.0)) +
                                 (6.0 * sqrt(O / ((sqrt(3.0) * B + 3.0) * (sqrt(3.0) * B + 3.0)))) + (2.0 * A + sqrt(3.0))) /
                                (sqrt(3.0) * B + 3.0));


      /* double gamma      = CV_PI - (delta + Alpha); */

      /* double distMiddle = sin(gamma) * armLength / sin(Alpha); */
      double distMiddle=(armLength*sin(M_PI-(delta+Alpha)))/(sin(Alpha));


      double l = sqrt(fmax(0.1, distMiddle * distMiddle + armLength * armLength - 2 * distMiddle * armLength * cos(delta + (CV_PI / 3.0))));

      double Epsilon=asin((armLength/l)*sin(delta+M_PI/3));
      /* phi=asin((b/l)*sin(delta+pi/3)); */

      /* double phi = asin(sin(delta + (CV_PI / 3.0)) * (armLength / l)); */
      double phi = asin(sin(delta + (CV_PI / 3.0)) * (distMiddle / l));
      std::cout << "delta: " << delta << std::endl;
      std::cout << "Estimated distance: " << l << std::endl;
      /* std_msgs::Float32 dM, fdM; */
      /* dM.data  = distance; */
      /* fdM.data = distanceFiltered; */
      /* measuredDist.publish(dM); */
      /* filteredDist.publish(fdM); */

      std::cout << "Estimated angle from mid. LED: " << phi * (180.0 / CV_PI) << std::endl;

      double C=acos(V2_c.dot(V2));
      Eigen::Vector3d V2_d=V2_c-V2;
      if (V2_d(1)<0)
        C=-C;
      double t=acos(V1.dot(V3));

      double Omega1=asin(fmax(-1.0,fmin(1.0,(C/t)*(2.0*sqrt(3.0)))));

      /* Eigen::Vector3d Pv = V2.cross(V1).normalized(); */
      /* Rc = makehgtform('axisrotate',norm13,epsilon); */
      Eigen::Transform< double, 3, Eigen::Affine > Rc(Eigen::AngleAxis< double >(Epsilon, norm13));
      /* vc=Rc(1:3,1:3)*v2_c; */
      Eigen::Vector3d Vc = Rc*V2_c;

      Eigen::VectorXd Yt=l*Vc;
      /* goalInCam        = (distanceFiltered - followDistance) * (Rp * V2); */
      /* tf::Vector3 centerEstimInCamTF; */
      /* tf::vectorEigenToTF(centerEstimInCam, centerEstimInCamTF); */
      /* tf::Vector3 centerEstimInBaseTF = (transform * centerEstimInCamTF); */
      /* /1* std::cout << centerEstimInBaseTF << std::endl; *1/ */
      /* tf::vectorTFToEigen(centerEstimInBaseTF, centerEstimInBase); */



      /* std::cout << "Estimated center in CAM: " << Yt << std::endl; */

      double tilt_perp=Omega1+atan2(Vc(1),sqrt(sqr(Vc(2))+sqr(Vc(0))));

      double relyaw;

      ROS_INFO_STREAM("leds: " << id);

      if (expFrequencies.size() == 2){
        if     ((id(0)==ids[0]) && (id(1)==ids[0]) && (id(2)==ids[0]))
          relyaw=(M_PI/2);
        else if ((id(0)==ids[1]) && (id(1)==ids[1]) && (id(2)==ids[1]))
        relyaw=(-M_PI/2);
        else if ((id(0)==ids[0]) && (id(1)==ids[0]) && (id(2)==ids[1]))
        relyaw=(M_PI/6);
        else if ((id(0)==ids[0]) && (id(1)==ids[1]) && (id(2)==ids[1]))
        relyaw=(-M_PI/6);
        else if ((id(0)==ids[1]) && (id(1)==ids[1]) && (id(2)==ids[0]))
        relyaw=(-5*M_PI/6);
        else if ((id(0)==ids[1]) && (id(1)==ids[0]) && (id(2)==ids[0]))
        relyaw=(5*M_PI/6);
        else
          if (id(0)==ids[0])
            relyaw=(M_PI/2)+ambig;
          else
            relyaw=(-M_PI/2)+ambig;
      }
      else {
        if     ((id(0)==ids[2]) && (id(1)==ids[0]) && (id(2)==ids[0]))
          relyaw=(M_PI/2);
        else if ((id(0)==ids[1]) && (id(1)==ids[1]) && (id(2)==ids[2]))
        relyaw=(-M_PI/2);
        else if ((id(0)==ids[0]) && (id(1)==ids[0]) && (id(2)==ids[1]))
        relyaw=(M_PI/6);
        else if ((id(0)==ids[0]) && (id(1)==ids[1]) && (id(2)==ids[1]))
        relyaw=(-M_PI/6);
        else if ((id(0)==ids[1]) && (id(1)==ids[2]) && (id(2)==ids[2]))
        relyaw=(-5*M_PI/6);
        else if ((id(0)==ids[2]) && (id(1)==ids[2]) && (id(2)==ids[0]))
          relyaw=(5*M_PI/6);
        else
          if (id(0)==ids[0])
            relyaw=(M_PI/2)+ambig;
          else
            relyaw=(-M_PI/2)+ambig;
      }

      double latnorm=sqrt(sqr(Yt(0))+sqr(Yt(2)));
      double Gamma=atan2(Yt(1),latnorm);
      Eigen::Vector3d obs_normal =V3.cross(V1); //not exact, but in practice changes very little
      obs_normal=obs_normal/(obs_normal.norm());
      Eigen::Vector3d latComp;
      latComp << Vc(0),0,Vc(2);
      latComp = latComp/(latComp.norm());
      if (Vc(1)<0) latComp = -latComp;

      /* Re = makehgtform('axisrotate',cross(vc,latComp),Gamma+pi/2); */
      Eigen::Transform< double, 3, Eigen::Affine > Re(Eigen::AngleAxis< double >( Gamma+(M_PI/2),(Vc.cross(latComp)).normalized()));
      Eigen::Vector3d exp_normal=Re*Vc;
      /* Ro = makehgtform('axisrotate',cross(vc,obs_normal),Gamma); */
      Eigen::Transform< double, 3, Eigen::Affine > Ro(Eigen::AngleAxis< double >( Gamma,(Vc.cross(obs_normal)).normalized()));
      obs_normal=Ro*obs_normal;
      /* obs_normal=Ro(1:3,1:3)*obs_normal; */
      /* exp_normal=Re(1:3,1:3)*vc; */
      double tilt_par=acos(obs_normal.dot(exp_normal));
      if (V1(1)<V2(1))
        tilt_par=-tilt_par;


      ROS_INFO_STREAM("latComp: " << latComp);
      ROS_INFO_STREAM("Vc: " << Vc);
      ROS_INFO_STREAM("Gamma: " << Gamma);
      ROS_INFO_STREAM("exp_normal: " << exp_normal);
      ROS_INFO_STREAM("obs_normal: " << obs_normal);
      ROS_INFO_STREAM("tilt_par: " << tilt_par);
      ROS_INFO_STREAM("tilt_perp: " << tilt_perp);

      double relyaw_view=relyaw+phi;
      relyaw=relyaw-atan2(Vc(0),Vc(2))+phi;

      double xl=(armLength/2)*cos(phi);
      double dist = Yt.norm();
      /* % X(1:3)=Xt(1:3) */
      Eigen::VectorXd Y(6);
      Yt = Yt*((dist-xl)/dist);
      latnorm=sqrt(sqr(Y(0))+sqr(Y(2)));
      double latang=atan2(Yt(0),Yt(2));
      Y(1)=Yt(1)-xl*sin(tilt_perp)*cos(tilt_par);
      Y(0)=Yt(0)-xl*sin(tilt_perp)*sin(tilt_par)*cos(latang)+xl*cos(tilt_perp)*sin(latang);
      Y(2)=Yt(2)+xl*sin(tilt_perp)*sin(tilt_par)*sin(latang)+xl*cos(tilt_perp)*cos(latang);


      double reltilt_abs=atan(sqrt(sqr(tan(tilt_perp))+sqr(tan(tilt_par))));
      double tiltdir=atan2(-tan(tilt_par),tan(tilt_perp));
      double tiltdir_adj=tiltdir-relyaw_view;
      double ta=cos(tiltdir_adj);
      double tb=-sin(tiltdir_adj);
      double tc=cot(reltilt_abs);
      double tpitch=atan2(ta,tc);
      double troll=atan2(tb,tc);

      Y << Yt.x(),Yt.y(),Yt.z(),troll,tpitch,relyaw;
      return Y;
  }

  static Eigen::Matrix3d rotate_covariance(const Eigen::Matrix3d& covariance, const Eigen::Matrix3d& rotation) {
    return rotation * covariance * rotation.transpose();  // rotate the covariance to point in direction of est. position
  }

  //Courtesy of Matous Vrba
  static Eigen::Matrix3d calc_position_covariance(const Eigen::Vector3d& position_sf, const double xy_covariance_coeff, const double z_covariance_coeff) {
    Eigen::Matrix3d v_x;
    Eigen::Vector3d b;
    Eigen::Vector3d v;
    double sin_ab, cos_ab;
    Eigen::Matrix3d vec_rot;
    const Eigen::Vector3d a(0.0, 0.0, 1.0);
    /* Calculates the corresponding covariance matrix of the estimated 3D position */
    Eigen::Matrix3d pos_cov = Eigen::Matrix3d::Identity();  // prepare the covariance matrix
    /* const double tol = 1e-9; */
    pos_cov(0, 0) = pos_cov(1, 1) = xy_covariance_coeff;

    /* double dist = position_sf.norm(); */
    /* pos_cov(2, 2) = dist * sqrt(dist) * z_covariance_coeff; */
    /* if (pos_cov(2, 2) < 0.33 * z_covariance_coeff) */
    /*   pos_cov(2, 2) = 0.33 * z_covariance_coeff; */
    pos_cov(2, 2) = z_covariance_coeff;

    // Find the rotation matrix to rotate the covariance to point in the direction of the estimated position
    b = position_sf.normalized();
    v = a.cross(b);
    sin_ab = v.norm();
    cos_ab = a.dot(b);
    const double angle = atan2(sin_ab, cos_ab);     // the desired rotation angle
    /* ROS_INFO_STREAM("pi/2 about x: "<< Eigen::AngleAxisd(M_PI/2, Eigen::Vector3d(1,0,0)).toRotationMatrix()); */
    vec_rot = Eigen::AngleAxisd(angle, v.normalized()).toRotationMatrix();
    pos_cov = rotate_covariance(pos_cov, vec_rot);  // rotate the covariance to point in direction of est. position
    if (pos_cov.array().isNaN().any()){
      ROS_INFO_STREAM("NAN IN  COVARIANCE!!!!");
      ROS_INFO_STREAM("pos_cov: \n" <<pos_cov);
      /* ROS_INFO_STREAM("v_x: \n" <<v_x); */
      ROS_INFO_STREAM("sin_ab: " <<sin_ab);
      ROS_INFO_STREAM("cos_ab: " <<cos_ab);
      ROS_INFO_STREAM("vec_rot: \n"<<vec_rot);
      ROS_INFO_STREAM("a: " <<a.transpose());
      ROS_INFO_STREAM("b: " <<b.transpose());
      ROS_INFO_STREAM("v: " <<v.transpose());
    }
    return pos_cov;
  }

  unscented::measurement uvdarHexarotorPose1p_meas(Eigen::Vector2d X,double tubewidth, double tubelength, double meanDist){
    double v1[3];
    double x[2] = {X.y(),X.x()};
    cam2world(v1, x, &oc_model);
    Eigen::Vector3d V1(v1[1], v1[0], -v1[2]);
    V1 = V1*meanDist;

    unscented::measurement ms;
    ms.x = Eigen::VectorXd(6);
    ms.C = Eigen::MatrixXd(6,6);

    ms.x << V1.x(),V1.y(),V1.z(),0,0,0; 
    Eigen::MatrixXd temp;
    temp.setIdentity(6,6);
    ms.C = temp*666;//large covariance for angles in radians
    ms.C.topLeftCorner(3, 3) = calc_position_covariance(V1,tubewidth,tubelength);

    /* std::cout << "ms.C: " << ms.C << std::endl; */


    return ms;

  }

  Eigen::VectorXd uvdarHexarotorPose2p(Eigen::VectorXd X, Eigen::VectorXd expFrequencies){

    /* ROS_INFO_STREAM("X: " << X); */

      cv::Point3d a;
      cv::Point3d b;

      if ((X(0)) < (X(3))) {
        a = cv::Point3d(X(0),X(1),X(2));
        b = cv::Point3d(X(3),X(4),X(5));
      } else {
        a = cv::Point3d(X(3),X(4),X(5));
        b = cv::Point3d(X(0),X(1),X(2));
      }
      double ambig=X(7);
      double delta=X(6);

      /* std::cout << "right led: " << b << std::endl; */
      Eigen::Vector3i ids;
      ids << 0,1,2;
      Eigen::Vector3d expPeriods;
      expPeriods = expFrequencies.cwiseInverse(); 
      Eigen::Vector3d periods;
      periods << a.z,b.z;
      Eigen::Vector3d id;
      Eigen::MatrixXd::Index   minIndex;
      ((expPeriods.array()-(periods(0))).cwiseAbs()).minCoeff(&minIndex);
      id(0) = minIndex;
      ((expPeriods.array()-(periods(1))).cwiseAbs()).minCoeff(&minIndex);
      id(1) = minIndex;




      cv::Point3d central = (a+b) / 2.0;
      double      v1[3], v2[3];
      double      va[2] = {double(a.y), double(a.x)};
      double      vb[2] = {double(b.y), double(b.x)};
      ;
      cam2world(v1, va, &oc_model);
      cam2world(v2, vb, &oc_model);
      /* double vc[3]; */
      /* double pc[2] = {central.y, central.x}; */
      /* cam2world(vc, pc, &oc_model); */

      Eigen::Vector3d V1(v1[1], v1[0], -v1[2]);
      Eigen::Vector3d V2(v2[1], v2[0], -v2[2]);
      /* Eigen::Vector3d Vc(vc[1], vc[0], -vc[2]); */

      /* double alpha = acos(V1.dot(V2)); */

      /* double vd = sqrt(0.75 * armLength); */

      /* double distance = (armLength / 2.0) / tan(alpha / 2.0) + vd; */
      /* if (first) { */
      /*   distanceSlider.filterInit(distance, filterDistLength); */
      /*   orientationSlider.filterInit(angleDist, filterOrientationLength); */
      /*   first = false; */
      /* } */
      double d = armLength;
      double v=d*sqrt(3.0/4.0);
      double sqv=v*v;
      double sqd=d*d;
      double csAlpha = (V1.dot(V2));
      double Alpha=acos(csAlpha);
      double Alpha2=Alpha*Alpha;
      double snAlpha =sin(Alpha);
      double sndelta =sin(delta);
      double sn2delta =sin(2*delta);
      double csdelta =cos(delta);
      double cs2delta =cos(2*delta);

      /* ROS_INFO("Alpha: %f, v: %f, d: %f, delta: %f",Alpha, v, d, delta); */
      /* ROS_INFO_STREAM("V1:" << V1 << std::endl <<"V2: " << V2); */

      double l =
        (4*d*v*Alpha - 
         sqd*csAlpha - sqd*cos(Alpha - 2*delta) - 
         6*d*v*snAlpha - 2*d*v*sin(Alpha - 2*delta) + 
         sqd*Alpha*sn2delta + 4*sqv*Alpha*sn2delta - 
         sqrt(2)*sqrt(
           sqr(d*csdelta - 2*v*sndelta)*
           (
            sqd - sqd*Alpha2 - 4*sqv*Alpha2 - 4*d*v*Alpha*csAlpha + 
            4*d*v*Alpha*cos(Alpha - 2*delta) + 
            sqd*cos(2*(Alpha - delta)) - 
            sqd*Alpha2*cs2delta + 
            4*sqv*Alpha2*cs2delta + 
            2*sqd*Alpha*snAlpha + 
            2*sqd*Alpha*sin(Alpha - 2*delta) - 
            4*d*v*Alpha2*sn2delta)))
        /
        (4*d*csdelta*(Alpha - 2*snAlpha) + 8*v*Alpha*sndelta);

      /* distanceSlider.filterPush(distance); */
      /* orientationSlider.filterPush(angleDist); */

      /* std::cout << "Estimated distance: " << l << std::endl; */
      /* std::cout << "Filtered distance: " << distanceFiltered << std::endl; */

      /* std::cout << "Estimated direction in CAM: " << (Rp*V2) << std::endl; */
      /* std::cout << "Central LED direction in CAM: " << (V2) << std::endl; */
      /* std::cout << "Rotation: " << Rp.matrix()   << std::endl; */

      /* foundTarget = true; */
      /* lastSeen    = ros::Time::now(); */

      /* std_msgs::Float32 dM, fdM; */
      /* dM.data  = distance; */
      /* fdM.data = distanceFiltered; */
      /* measuredDist.publish(dM); */
      /* filteredDist.publish(fdM); */



      double kappa = M_PI/2-delta;
      double d1=(d/2)+v*tan(delta);
      double xl=v/cos(delta);
      double yl=l-xl;
      double alpha1=atan((d1*sin(kappa))/(yl-d1*cos(kappa)));
      Eigen::Vector3d Pv = V2.cross(V1).normalized();
      Eigen::Transform< double, 3, Eigen::Affine > Rp(Eigen::AngleAxis< double >(-alpha1, Pv));
      /* Rc = makehgtform('axisrotate',cross(v2,v1),-alpha1); */
      Eigen::Vector3d Vc=Rp*V1;
      Eigen::Vector3d Yt=l*Vc;

      /* std::cout << "Estimated center in CAM: " << Yt << std::endl; */
      /* geometry_msgs::Pose p; */
      /* p.position.x = centerEstimInCam.x(); */
      /* p.position.y = centerEstimInCam.y(); */
      /* p.position.z = centerEstimInCam.z(); */
      /* targetInCamPub.publish(p); */
      /* foundTarget = true; */
      /* lastSeen    = ros::Time::now(); */

      double relyaw;

      if (expFrequencies.size() == 2)
        if     ((id(0)==ids[0]) && (id(1)==ids[0]))
          relyaw=(M_PI/2)+ambig+delta;
        else if ((id(0)==ids[1]) && (id(1)==ids[1]))
          relyaw=(-M_PI/2)+ambig+delta;
        else if ((id(0)==ids[0]) && (id(1)==ids[1]))
          relyaw=0+delta;
        else
          relyaw=M_PI+delta;

        else 
          if     ((id(0)==ids[0]) && (id(1)==ids[0]))
            relyaw=(M_PI/3)+delta;
          else if ((id(0)==ids[1]) && (id(1)==ids[1]))
            relyaw=(-M_PI/3)+delta;
          else if ((id(0)==ids[0]) && (id(1)==ids[1]))
            relyaw=0+delta;
          else if ((id(0)==ids[1]) && (id(1)==ids[2]))
            relyaw=(-2*M_PI/3)+delta;
          else if ((id(0)==ids[2]) && (id(1)==ids[0]))
            relyaw=(2*M_PI/3)+delta;
          else if ((id(0)==ids[2]) && (id(1)==ids[2]))
            relyaw=(M_PI)+delta;
          else
            relyaw=ambig+delta;
        
      double latang=atan2(Vc(0),Vc(2));

      double relyaw_view=relyaw;

      /* ROS_INFO_STREAM("expFrequencies: " << expFrequencies); */
      /* ROS_INFO_STREAM("expPeriods: " << expPeriods); */
      /* ROS_INFO_STREAM("periods: " << periods); */
      /* ROS_INFO_STREAM("id: " << id); */
      /* ROS_INFO("relyaw_orig: %f",relyaw); */
      relyaw=relyaw-latang;
      /* ROS_INFO_STREAM("Vc: " << Vc); */
      /* ROS_INFO("latang: %f",latang); */

      double latnorm=sqrt(sqr(Yt(0))+sqr(Yt(2)));
      double Gamma=atan2(Yt(1),latnorm);
      double tilt_perp=X(8);
      Eigen::Vector3d obs_normal=V2.cross(V1);
      obs_normal=obs_normal/(obs_normal.norm());
      Eigen::Vector3d latComp;
      latComp << Vc(0),0,Vc(2);
      latComp = latComp/(latComp.norm());

      if (Vc(1)<0) latComp = -latComp;
      /* ROS_INFO_STREAM("cross: " << Vc.cross(latComp)); */

      Eigen::Transform< double, 3, Eigen::Affine > Re(Eigen::AngleAxis< double >( Gamma+(M_PI/2),(Vc.cross(latComp)).normalized()));
      Eigen::Vector3d exp_normal=Re*Vc;
      /* Ro = makehgtform('axisrotate',cross(vc,obs_normal),Gamma); */
      Eigen::Transform< double, 3, Eigen::Affine > Ro(Eigen::AngleAxis< double >( Gamma,(Vc.cross(obs_normal)).normalized()));
      obs_normal=Ro*obs_normal;
      /* Re = makehgtform('axisrotate',cross(vc,latComp),Gamma+pi/2); */
      double tilt_par=acos(obs_normal.dot(exp_normal));
      if (V1(1)<V2(1))
        tilt_par=-tilt_par;


/*       ROS_INFO_STREAM("exp_normal: " << exp_normal); */
/*       ROS_INFO_STREAM("obs_normal: " << obs_normal); */
/*       ROS_INFO_STREAM("tilt_par: " << tilt_par); */

      double dist = Yt.norm();
      Yt= Yt*((dist-xl)/dist);
      Yt(1)=Yt(1)-xl*sin(Gamma+tilt_perp)*cos(tilt_par);
      Yt(0)=Yt(0)-xl*sin(Gamma+tilt_perp)*sin(tilt_par)*cos(latang)+xl*cos(Gamma+tilt_perp)*sin(latang);
      Yt(2)=Yt(2)+xl*sin(Gamma+tilt_perp)*sin(tilt_par)*sin(latang)+xl*cos(Gamma+tilt_perp)*cos(latang);


      double reltilt_abs=atan(sqrt(sqr(tan(tilt_perp))+sqr(tan(tilt_par))));
      double tiltdir=atan2(-tan(tilt_par),tan(tilt_perp));
      double tiltdir_adj=tiltdir-relyaw_view;
      double ta=cos(tiltdir_adj);
      double tb=-sin(tiltdir_adj);
      double tc=cot(reltilt_abs);
      double tpitch=atan2(ta,tc);
      double troll=atan2(tb,tc);

      Eigen::VectorXd Y(6);
      Y << Yt.x(),Yt.y(),Yt.z(),troll,tpitch,relyaw;

      return Y;

       
    }



  void TfThread() {
    gotCam2Base = false;
    ros::Rate transformRate(1.0);

    for (std::string cameraFrame: cameraFrames) {
      while (true) {
        transformRate.sleep();

        try {
          listener.waitForTransform("fcu_" + uav_name, cameraFrame, ros::Time::now(), ros::Duration(1.0));
          mutex_tf.lock();
          listener.lookupTransform("fcu_" + uav_name, cameraFrame, ros::Time(0), transformCam2Base);
          mutex_tf.unlock();
        }
        catch (tf::TransformException ex) {
          ROS_ERROR("TF: %s", ex.what());
          mutex_tf.unlock();
          ros::Duration(1.0).sleep();
          continue;
        }
        break;;
      }
    }

    gotCam2Base = true;
  }

  //for legacy reasons
  void ProcessPointsUnstamped(const std_msgs::Int32MultiArrayConstPtr& msg, size_t imageIndex){
    if (DEBUG)
      ROS_INFO_STREAM("Getting message: " << *msg);
    uvdar::Int32MultiArrayStampedPtr msg_stamped(new uvdar::Int32MultiArrayStamped);
    msg_stamped->stamp = ros::Time::now()-ros::Duration(_legacy_delay);
    msg_stamped->layout= msg->layout;
    msg_stamped->data = msg->data;
    ProcessPoints(msg_stamped, imageIndex);
  }

  void ProcessPoints(const uvdar::Int32MultiArrayStampedConstPtr& msg, size_t imageIndex) {
    int                        countSeen;
    std::vector< cv::Point3i > points;
    lastBlinkTime = msg->stamp;
    countSeen = (int)((msg)->layout.dim[0].size);
    if (DEBUG)
      ROS_INFO("Received points: %d", countSeen);
    if (countSeen < 1) {
      foundTarget = false;
      return;
    }
    if (estimatedFramerate.size() <= imageIndex || estimatedFramerate[imageIndex] < 0) {
      ROS_INFO("Framerate is not yet estimated. Waiting...");
      return;
    }
    if (!gotCam2Base) {
      ROS_INFO("Transformation to base is missing...");
      return;
    }

    for (int i = 0; i < countSeen; i++) {
      if (mask_active)
        if (mask.at<unsigned char>(cv::Point2i(msg->data[(i * 3)], msg->data[(i * 3) + 1])) < 100){
          /* if (DEBUG) */
          ROS_INFO_STREAM("Discarding point " << cv::Point3i(msg->data[(i * 3)], msg->data[(i * 3) + 1], msg->data[(i * 3) + 2]));
          continue;
        }

      if (msg->data[(i * 3) + 2] <= 200) {
        points.push_back(cv::Point3i(msg->data[(i * 3)], msg->data[(i * 3) + 1], msg->data[(i * 3) + 2]));
      }
    }
    
    std::vector<std::vector<cv::Point3i>> separatedPoints;
    separatedPoints.resize(targetCount);

    if (points.size() > 0) {

      for (int i = 0; i < points.size(); i++) {
        if (points[i].z > 1) {
          int mid = findMatch(points[i].z);
          int tid = classifyMatch(mid);
          ROS_INFO("[%s]: FR: %d, MID: %d, TID: %d", ros::this_node::getName().c_str(),points[i].z, mid, tid);
          /* separatedPoints[classifyMatch(findMatch(points[i].z))].push_back(points[i]); */
          if (tid>=0)
            separatedPoints[tid].push_back(points[i]);
        }
      }

      for (int i = 0; i < targetCount; i++) {
        ROS_INFO_STREAM("target [" << i << "]: ");
        ROS_INFO_STREAM("p: " << separatedPoints[i]);
        extractSingleRelative(separatedPoints[i], i, imageIndex);
      }
    }
  }

  void extractSingleRelative(std::vector< cv::Point3i > points, int target, size_t imageIndex) {
    
    double leftF = frequencySet[target*2];
    double rightF = frequencySet[target*2+1];
    Eigen::Vector3d centerEstimInCam;
    double          maxDist = maxDistInit;

    int countSeen = (int)(points.size());


    if (points.size() > 1) {

      for (int i = 0; i < points.size(); i++) {
        if (points[i].z < 1) {
          points.erase(points.begin() + i);
          i--;
          continue;
        }
      }
      bool separated = false;
      while ((points.size() > 3) || (!separated)) {
        separated = true;
        for (int i = 0; i < points.size(); i++) {
          bool viable = false;
          for (int j = 0; j < points.size(); j++) {
            if (i == j)
              continue;

            /* if ((cv::norm(points[i] - points[j]) < maxDist) && (abs(points[i].y - points[j].y) < abs(points[i].x - points[j].x))) { */
            ROS_INFO_STREAM("Distance: " <<cv::norm(cv::Point2i(points[i].x,points[i].y) - cv::Point2i(points[j].x,points[j].y)) << " maxDist: " << maxDist);

            if ((cv::norm(cv::Point2i(points[i].x,points[i].y) - cv::Point2i(points[j].x,points[j].y)) < maxDist)) {
              viable = true;
            }
            else{
              separated = false;
            }
          }
          if (!viable) {
            points.erase(points.begin() + i);
            i--;
          }
        }
        maxDist = 0.5 * maxDist;
        /* std::cout << "maxDist: " << maxDist << std::endl; */
      }
    }


      unscented::measurement ms;

      ROS_INFO_STREAM("framerateEstim: " << estimatedFramerate[imageIndex]);

      double perr=0.2/estimatedFramerate[imageIndex];

    if (points.size() == 3) {
      ROS_INFO_STREAM("points: " << points);
      X3 <<
        points[0].x ,points[0].y, 1.0/(double)(points[0].z),
        points[1].x ,points[1].y, 1.0/(double)(points[1].z),
        points[2].x, points[2].y, 1.0/(double)(points[2].z),
        0;  //to account for ambiguity
      Px3 <<
        qpix ,0,0,0,0,0,0,0,0,0,
        0,qpix ,0,0,0,0,0,0,0,0,
        0,0,sqr(perr),0,0,0,0,0,0,0,
        0,0,0,qpix ,0,0,0,0,0,0,
        0,0,0,0,qpix ,0,0,0,0,0,
        0,0,0,0,0,sqr(perr),0,0,0,0,
        0,0,0,0,0,0,qpix ,0,0,0,
        0,0,0,0,0,0,0,qpix ,0,0,
        0,0,0,0,0,0,0,0,sqr(perr),0,
        0,0,0,0,0,0,0,0,0,sqr(2*M_PI/3)
        ;
      ROS_INFO_STREAM("X3: " << X3);
      boost::function<Eigen::VectorXd(Eigen::VectorXd,Eigen::VectorXd)> callback;
      callback=boost::bind(&PoseReporter::uvdarHexarotorPose3p,this,_1,_2);
      ms = unscented::unscentedTransform(X3,Px3,callback,leftF,rightF,-1);
    }
    else if (points.size() == 2) {
      ROS_INFO_STREAM("points: " << points);
      X2 <<
        (double)(points[0].x) ,(double)(points[0].y),1.0/(double)(points[0].z),
        (double)(points[1].x) ,(double)(points[1].y),1.0/(double)(points[1].z),
        0.0,0.0,0.0;
      Px2 <<
        qpix ,0,0,0,0,0,0,0,0,
        0,qpix ,0,0,0,0,0,0,0,
        0,0,sqr(perr),0,0,0,0,0,0,
        0,0,0,qpix ,0,0,0,0,0,
        0,0,0,0,qpix ,0,0,0,0,
        0,0,0,0,0,sqr(perr),0,0,0,
        0,0,0,0,0,0,sqr(deg2rad(8)),0,0,
        0,0,0,0,0,0,0,sqr(deg2rad(45)),0,
        0,0,0,0,0,0,0,0,sqr(deg2rad(10))
        ;

      ROS_INFO_STREAM("X2: " << X2);
      boost::function<Eigen::VectorXd(Eigen::VectorXd,Eigen::VectorXd)> callback;
      callback=boost::bind(&PoseReporter::uvdarHexarotorPose2p,this,_1,_2);
      ms = unscented::unscentedTransform(X2,Px2,callback,leftF,rightF,-1);
    }

    else if (points.size() == 1) {
      std::cout << "Only single point visible - no distance information" << std::endl;
      angleDist = 0.0;
      std::cout << "led: " << points[0] << std::endl;


      ms = uvdarHexarotorPose1p_meas(Eigen::Vector2d(points[0].x,points[0].y),armLength, 500,10.0);


      foundTarget = true;
      lastSeen    = ros::Time::now();
    } else {
      std::cout << "No valid points seen. Waiting" << std::endl;
      centerEstimInCam.x()  = 0;
      centerEstimInCam.y()  = 0;
      centerEstimInCam.z()  = 0;
      centerEstimInBase.x() = 0;
      centerEstimInBase.y() = 0;
      centerEstimInBase.z() = 0;
      tailingComponent      = 0;
      foundTarget           = false;
      return;
    }


    /* Eigen::MatrixXd iden(6,6); */
    /* iden.setIdentity(); */
    /* ms.C +=iden*0.1; */

    ROS_INFO_STREAM("Y: \n" << ms.x );
    /* std::cout << "Py: " << ms.C << std::endl; */
    ROS_INFO_STREAM("Py: \n" << ms.C );

    msgOdom = boost::make_shared<geometry_msgs::PoseWithCovarianceStamped>();;
    /* msgOdom->twist.covariance = msgOdom->pose.covariance; */

    /* geometry_msgs::PoseWithCovarianceStampedPtr msgPose = boost::make_shared<geometry_msgs::PoseWithCovarianceStamped>();; */
    /* geometry_msgs::PoseWithCovariancePtr msgPose = boost::make_shared<geometry_msgs::PoseWithCovariance>();; */
    /* msgPose->header.frame_id ="uvcam"; */
    /* msgPose->header.stamp = ros::Time::now(); */
    msgOdom->pose.pose.position.x = ms.x(0);
    msgOdom->pose.pose.position.y = ms.x(1);
    msgOdom->pose.pose.position.z = ms.x(2);
    tf::Quaternion qtemp;
    qtemp.setRPY(ms.x(3), ms.x(4), ms.x(5));
    qtemp=(transformCam2Base.getRotation().inverse())*qtemp;
    /* Eigen::Affine3d aTtemp; */
    /* tf::transformTFToEigen(transformCam2Base, aTtemp); */
    /* qtemp = aTtemp*qtemp; */
    msgOdom->pose.pose.orientation.x = qtemp.x();
    msgOdom->pose.pose.orientation.y = qtemp.y();
    msgOdom->pose.pose.orientation.z = qtemp.z();
    msgOdom->pose.pose.orientation.w = qtemp.w();
    for (int i=0; i<ms.C.cols(); i++){
      for (int j=0; j<ms.C.rows(); j++){
        msgOdom->pose.covariance[ms.C.cols()*j+i] =  ms.C(j,i);
      }
    }

    msgOdom->header.frame_id = cameraFrames[imageIndex];
    msgOdom->header.stamp = lastBlinkTime;
    /* msgOdom->pose = *(msgPose); */

    /* msgOdom->twist.twist.linear.x = 0.0; */
    /* msgOdom->twist.twist.linear.y = 0.0; */
    /* msgOdom->twist.twist.linear.z = 0.0; */
    /* msgOdom->twist.twist.angular.x = 0.0; */
    /* msgOdom->twist.twist.angular.y = 0.0; */
    /* msgOdom->twist.twist.angular.z = 0.0; */
    
    /* msgOdom->twist.covariance = { */
    /*   4,0,0,0,0,0, */
    /*   0,4,0,0,0,0, */
    /*   0,0,4,0,0,0, */
    /*   0,0,0,2,0,0, */
    /*   0,0,0,0,2,0, */
    /*   0,0,0,0,0,2}; */


    measuredPose[target].publish(msgOdom);

    tf::Vector3 goalInCamTF, centerEstimInCamTF;
    /* tf::vectorEigenToTF(goalInCam, goalInCamTF); */
    tf::vectorEigenToTF(centerEstimInCam, centerEstimInCamTF);
    mutex_tf.lock();
    tf::Vector3 goalInBaseTF        = (transformCam2Base * goalInCamTF);
    tf::Vector3 centerEstimInBaseTF = (transformCam2Base * centerEstimInCamTF);
    mutex_tf.unlock();
    Eigen::Affine3d eigenTF;
    /* ROS_INFO("TF parent: %s", transform.frame_id_.c_str()); */
    /* std::cout << "fcu_" + uav_name << std::endl; */
    /* tf::transformTFToEigen(transform, eigenTF); */
    /* std::cout << "TF mat: " << eigenTF.matrix() << std::endl; */
    tf::vectorTFToEigen(goalInBaseTF, goalInBase);
    tf::vectorTFToEigen(centerEstimInBaseTF, centerEstimInBase);

    Eigen::Vector3d CEBFlat(centerEstimInBase);
    double          flatLen = sqrt(CEBFlat.x() * CEBFlat.x() + CEBFlat.y() * CEBFlat.y());
    CEBFlat                 = CEBFlat / flatLen;

    geometry_msgs::Pose p;
    std::cout << "Center in BASE: " << centerEstimInBase << std::endl;
    p.position.x = centerEstimInBase.x();
    p.position.y = centerEstimInBase.y();
    p.position.z = centerEstimInBase.z();

    /* if (reachedTarget) */
    /*   ROS_INFO("Reached target"); */
    /* tf::Vector3 centerEstimInCamTF; */
    /* tf::vectorEigenToTF(centerEstimInCam, centerEstimInCamTF); */
    /* tf::Vector3 centerEstimInBaseTF = (transform * centerEstimInCamTF); */
    /* tf::vectorTFToEigen(centerEstimInBaseTF, centerEstimInBase); */
    /* std::cout << "Estimated center in BASE: " << centerEstimInBase << std::endl; */
  }

  template < typename T >
  int sgn(T val) {
    return (T(0) < val) - (val < T(0));
  }

  int findMatch(double i_frequency) {
    double period = 1.0 / i_frequency;
    for (int i = 0; i < periodSet.size(); i++) {
      /* std::cout << period << " " <<  periodBoundsTop[i] << " " << periodBoundsBottom[i] << " " << periodSet[i] << std::endl; */
      if ((period > periodBoundsBottom[i]) && (period < periodBoundsTop[i])) {
        return i;
      }
    }
    return -1;
  }

  int classifyMatch(int ID) {
    return ID/frequenciesPerTarget;
  }

  void prepareFrequencyClassifiers() {
    for (int i = 0; i < frequencySet.size(); i++) {
      periodSet.push_back(1.0 / frequencySet[i]);
    }
    for (int i = 0; i < frequencySet.size() - 1; i++) {
      periodBoundsBottom.push_back((periodSet[i] * (1.0 - boundary_ratio) + periodSet[i + 1] * boundary_ratio));
    }
    periodBoundsBottom.push_back(1.0 / max_frequency);


    periodBoundsTop.push_back(1.0 / min_frequency);
    for (int i = 1; i < frequencySet.size(); i++) {
      periodBoundsTop.push_back((periodSet[i] * boundary_ratio + periodSet[i - 1] * (1.0 - boundary_ratio)));
    }


    /* periodBoundsTop.back()           = 0.5 * periodSet.back() + 0.5 * periodSet[secondToLast]; */
    /* periodBoundsBottom[secondToLast] = 0.5 * periodSet.back() + 0.5 * periodSet[secondToLast]; */
  }

private:

  void setMask(std::string mask_file){
    if (std::experimental::filesystem::exists(mask_file)){
      ROS_INFO_STREAM("[PoseReporter]: Setting mask to " << mask_file);
      mask = cv::imread(mask_file,CV_LOAD_IMAGE_GRAYSCALE);
      mask_active = true;
    }
    else
      ROS_INFO_STREAM("[PoseReporter]: Mask file " << mask_file << " does not exist.");

  }


  bool mask_active;
  cv::Mat mask;


  bool stopped;

  bool Flip;

  ros::Time RangeRecTime;

  ros::Subscriber pointsSubscriberLegacy;
  ros::Subscriber framerateSubscriber;

  std::vector<std::string> cameraFrames;

  using blinkers_seen_callback_t = std::function<void (const uvdar::Int32MultiArrayStampedConstPtr& msg)>;
  std::vector<blinkers_seen_callback_t> blinkersSeenCallbacks;
  std::vector<ros::Subscriber> blinkersSeenSubscribers;

  using estimated_framerate_callback_t = std::function<void (const std_msgs::Float32ConstPtr& msg)>;
  std::vector<estimated_framerate_callback_t> estimatedFramerateCallbacks;
  std::vector<ros::Subscriber> estimatedFramerateSubscribers;

  std::vector<double> estimatedFramerate;

  tf::TransformListener listener;
  tf::StampedTransform  transformCam2Base;
  tf::StampedTransform  transformBase2World;


  ros::Time begin;

  // Input arguments
  bool DEBUG;
  bool justReport;
  int  threshVal;
  bool silent_debug;
  bool storeVideo;
  bool AccelerationBounding;
  // std::vector<double> camRot;
  double gamma;  // rotation of camera in the helicopter frame (positive)


  int samplePointSize;

  int cellSize;
  int cellOverlay;
  int surroundRadius;

  double tailingComponent;

  bool gui;

  int numberOfBins;

  bool cameraRotated;

  std::mutex mutex_tf;

  double max_px_speed_t;
  float  maxAccel;
  bool   checkAccel;

  std::string uav_name;

  ros::Time odomSpeedTime;
  float     speed_noise;

  bool gotCam2Base;

  ros::Time lastBlinkTime;


  /* uvLedDetect_gpu *uvdg; */

  // thread
  std::thread tf_thread;

  std::vector< sensor_msgs::Imu > imu_register;

  struct ocam_model oc_model;

  ros::ServiceClient             client;
  mrs_msgs::Vec4              tpnt;
  bool                           foundTarget;
  Eigen::Vector3d                centerEstimInBase;
  Eigen::Vector3d                goalInBase;

  /* bool   toRight;    // direction in which we should go to reach the tail */
  double angleDist;  // how large is the angle around the target between us and the tail

  Eigen::MatrixXd Px2,Px3;
  Eigen::VectorXd X2,X3;


  ros::Subscriber OdomSubscriber;
  ros::Subscriber DiagSubscriber;
  bool            reachedTarget;
  ros::Time       lastSeen;

  bool               followTriggered;
  ros::ServiceServer ser_trigger;

  std::vector<ros::Publisher> measuredPose;

  /* Lkf* trackers[2]; */

  /* nav_msgs::OdometryPtr msgOdom; */
  geometry_msgs::PoseWithCovarianceStampedPtr msgOdom;

  int frequenciesPerTarget;
  int targetCount;
  std::vector<double> frequencySet;
  std::vector<double> periodSet;
  std::vector<double> periodBoundsTop;
  std::vector<double> periodBoundsBottom;

  std::string _calib_file;
  std::string _mask_file;

  bool _legacy;
  double _legacy_delay;
};


int main(int argc, char** argv) {
  ros::init(argc, argv, "uvdar_reporter");
  ros::NodeHandle nodeA;
  PoseReporter        pr(nodeA);

  ROS_INFO("[PoseReporter]: UVDAR Pose reporter node initiated");

  ros::spin();

  return 0;
}
