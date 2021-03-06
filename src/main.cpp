#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "MPC.h"
#include "json.hpp"

using json = nlohmann::json;
const double Lf = 2.67; // the length from front to CoG that has a similar radius.

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.rfind("}]");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

// Evaluate a polynomial.
double polyeval(Eigen::VectorXd coeffs, double x) {
  double result = 0.0;
  for (int i = 0; i < coeffs.size(); i++) {
    result += coeffs[i] * pow(x, i);
  }
  return result;
}

// Fit a polynomial.
// Adapted from
// https://github.com/JuliaMath/Polynomials.jl/blob/master/src/Polynomials.jl#L676-L716
Eigen::VectorXd polyfit(Eigen::VectorXd xvals, Eigen::VectorXd yvals,
                        int order) {
  assert(xvals.size() == yvals.size());
  assert(order >= 1 && order <= xvals.size() - 1);
  Eigen::MatrixXd A(xvals.size(), order + 1);

  for (int i = 0; i < xvals.size(); i++) {
    A(i, 0) = 1.0;
  }

  for (int j = 0; j < xvals.size(); j++) {
    for (int i = 0; i < order; i++) {
      A(j, i + 1) = A(j, i) * xvals(j);
    }
  }

  auto Q = A.householderQr();
  auto result = Q.solve(yvals);
  return result;
}

int main() {
  uWS::Hub h;

  // MPC initialization
  MPC mpc;

  h.onMessage([&mpc](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    string sdata = string(data).substr(0, length);
    cout << sdata << endl;
    if (sdata.size() > 2 && sdata[0] == '4' && sdata[1] == '2') {
      string s = hasData(sdata);
      if (s != "") {
        auto j = json::parse(s);
        string event = j[0].get<string>();
        if (event == "telemetry") {
          // j[1] is the data JSON object
          vector<double> ptsx = j[1]["ptsx"];
          vector<double> ptsy = j[1]["ptsy"];
          double px = j[1]["x"];
          double py = j[1]["y"];
          double psi = j[1]["psi"];
          double v = j[1]["speed"];
          double delta = j[1]["steering_angle"];
          double a = j[1]["throttle"];

          // Calculate steering angl and throttle using MPC.
          // transform reference path into vehicle orientation
          Eigen::VectorXd ptsx_v(ptsx.size()); // x positions from vehicle coordinates
          Eigen::VectorXd ptsy_v(ptsy.size()); // y positions from vehicle coordinates

          for (int i = 0; i < ptsx.size(); i++) {
            double dx = ptsx[i] - px; // difference between reference x and current vehicle x
            double dy = ptsy[i] - py; // difference between reference y and current vehicle y

            ptsx_v[i] = dx * cos(psi) + dy * sin(psi);
            ptsy_v[i] = dy * cos(psi) - dx * sin(psi);
          }

          // fit a polynomial and get coefficients
          auto coeffs = polyfit(ptsx_v, ptsy_v, 3);

          double latency = 0.1; //   system latency
          double x_v   = 0;     //   x from vehicle coordinates
          double y_v   = 0;     //   y from vehicle coordinates
          double psi_v = 0;     // psi from vehicle coordinates

          // calculate current state in vehicle coordinates
          double cte = polyeval(coeffs, 0) - y_v;
          double epsi = psi_v - atan(coeffs[1] + 2*coeffs[2]*x_v + 3*coeffs[3]*x_v*x_v);

          // update state with latency
          double late_x = x_v + v * cos(psi_v) * latency;
          double late_y = y_v + v * sin(psi_v) * latency;
          double late_psi = psi_v - v / Lf * delta * latency;
          double late_v = v + a * latency;
          double late_cte = cte + v * sin(psi_v) * latency;
          double late_epsi = late_psi - atan(coeffs[1] + 2 * coeffs[2] * late_x + 3 * coeffs[3] * late_x * late_x) - (v / Lf * delta * latency);

          Eigen::VectorXd state(6);
          state << late_x, late_y, late_psi, late_v, late_cte, late_epsi;

          auto vars = mpc.Solve(state, coeffs);

          double steer_value = vars[6];
          double throttle_value = vars[7];

          json msgJson;
          // steer_value is divide by deg2rad(25) to transform it into [-1, 1] constraints.
          msgJson["steering_angle"] = -steer_value/deg2rad(25);
          msgJson["throttle"] = throttle_value;

          // Display the MPC predicted trajectory
          // the points are showed by green line in the simulator
          msgJson["mpc_x"] = mpc.next_path_xs;
          msgJson["mpc_y"] = mpc.next_path_ys;

          // Display the waypoints/reference line
          // the points are showed by yellow line in the simulator
          vector<double> next_x_vals;
          vector<double> next_y_vals;

          for (int i = 0; i < ptsx_v.size(); i++) {
            next_x_vals.push_back(ptsx_v[i]);
            next_y_vals.push_back(ptsy_v[i]);
          }

          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"steer\"," + msgJson.dump() + "]";
          std::cout << msg << std::endl;
          // Latency
          // The purpose is to mimic real driving conditions where
          // the car does actuate the commands instantly.
          this_thread::sleep_for(chrono::milliseconds(100));
          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
