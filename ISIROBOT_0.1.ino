#include <WiFi.h>
#include <WebServer.h>

// ================= WiFi =================
const char* apSsid = "ESP32_Car";
const char* apPassword = "12345678";
WebServer server(80);

// ================= L298N =================
#define IN1 14
#define IN2 27
#define IN3 26
#define IN4 25
#define ENA_PIN 33
#define ENB_PIN 32

// ================= PWM =================
const int PWM_FREQ = 20000;
const int PWM_RES  = 8;

// ================= Speed =================
uint8_t manualSpeed = 200;
uint8_t autoSpeed   = 200;

// ================= Ultrasonic ===============
#define TRIG_PIN 13
#define ECHO_PIN 12

// ================= Flags =================
bool forwardCmd = false;
bool backCmd    = false;
bool leftCmd    = false;
bool rightCmd   = false;

bool obstacleMode = false;
bool followMode   = false;

// Failsafe: if no command arrives for this long, the car stops
unsigned long lastCmdTime = 0;
const unsigned long CMD_TIMEOUT = 500;

// ================= Auto Obstacle =================
enum AutoState { AUTO_IDLE, AUTO_FORWARD, AUTO_BACKWARD, AUTO_TURN };
AutoState autoState = AUTO_IDLE;

unsigned long autoStateStart = 0;
bool turnLeftNext = true;

const unsigned long BACK_TIME = 600;
const unsigned long TURN_TIME = 700;
const float OBSTACLE_DIST_CM = 20.0;

// ================= HTML =================
const char index_html[] PROGMEM = R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<style>
  body{
    background:#111; color:#fff; font-family:Arial; text-align:center;
    user-select:none; -webkit-user-select:none;
    -webkit-touch-callout:none; -webkit-tap-highlight-color:transparent;
  }
  .btn{
    padding:16px 26px; margin:6px; border:none; border-radius:10px;
    background:#333; color:#fff; font-size:16px;
    touch-action:none;
  }
  .btn:active{background:#555}
  .btn.on{background:#2e7d32}
  .hold.pressed{background:#ff9800; color:#000}
  #st{margin:8px; font-size:15px; font-weight:bold; color:#aaa}
</style>
</head>
<body>

<h2>ESP32 Web Robot Car</h2>
<div id="st">...</div>

<button id="bf" class="btn hold">Forward</button><br>

<button id="bl" class="btn hold">Left</button>
<button class="btn" onclick="release();cmd('s',1)">Stop</button>
<button id="br" class="btn hold">Right</button><br>

<button id="bb" class="btn hold">Backward</button>

<h3>Obstacle Avoid Mode</h3>
<button id="o1" class="btn" onclick="mode(1)">ON</button>
<button id="o0" class="btn" onclick="mode(0)">OFF</button>

<h3>Follow Mode</h3>
<button id="f1" class="btn" onclick="follow(1)">FOLLOW ON</button>
<button id="f0" class="btn" onclick="follow(0)">FOLLOW OFF</button>

<script>
  var timer = null, cur = null;

  function setSt(ok){
    var s = document.getElementById('st');
    s.textContent = ok ? 'Connected' : 'NO CONNECTION';
    s.style.color = ok ? '#4caf50' : '#f44336';
  }

  function send(url){
    var x = new XMLHttpRequest();
    x.open('GET', url + '&t=' + Date.now(), true);
    x.timeout = 1500;
    x.onload = function(){ setSt(true); };
    x.onerror = function(){ setSt(false); };
    x.ontimeout = function(){ setSt(false); };
    x.send();
  }

  function cmd(d,s){ send('/cmd?dir=' + d + '&state=' + s); }

  function setActive(a){
    ['o1','o0','f1','f0'].forEach(function(i){
      document.getElementById(i).classList.toggle('on', a.indexOf(i) >= 0);
    });
  }
  function mode(v){ send('/mode?auto=' + v); setActive(v ? ['o1','f0'] : ['o0','f0']); }
  function follow(v){ send('/follow?f=' + v); setActive(v ? ['f1','o0'] : ['f0','o0']); }

  function press(b,d){
    release();
    cur = d;
    b.classList.add('pressed');
    cmd(d,1);
    timer = setInterval(function(){ cmd(d,1); }, 150);
  }
  function release(){
    if(timer){ clearInterval(timer); timer = null; }
    var all = document.querySelectorAll('.hold');
    for(var i = 0; i < all.length; i++) all[i].classList.remove('pressed');
    if(cur){ var d = cur; cur = null; cmd(d,0); }
  }
  function bindHold(id,d){
    var b = document.getElementById(id);
    b.addEventListener('pointerdown', function(e){
      e.preventDefault();
      try{ b.setPointerCapture(e.pointerId); }catch(x){}
      press(b,d);
    });
    b.addEventListener('pointerup', release);
    b.addEventListener('pointercancel', release);
    b.addEventListener('contextmenu', function(e){ e.preventDefault(); });
  }
  bindHold('bf','f'); bindHold('bl','l'); bindHold('br','r'); bindHold('bb','b');
  window.addEventListener('blur', release);
  document.addEventListener('visibilitychange', release);

  mode(0);
</script>

</body>
</html>)=====";

// ================= Motor =================
bool wasStopped = true;
const unsigned long KICK_TIME = 200;

void setMotorSpeed(uint8_t l, uint8_t r){
  if(l==0 && r==0){
    wasStopped = true;
    ledcWrite(ENA_PIN, 0);
    ledcWrite(ENB_PIN, 0);
    return;
  }
  if(wasStopped){
    wasStopped = false;
    ledcWrite(ENA_PIN, 255);
    ledcWrite(ENB_PIN, 255);
    delay(KICK_TIME);
  }
  ledcWrite(ENA_PIN, l);
  ledcWrite(ENB_PIN, r);
}

void driveStop(){
  digitalWrite(IN1,LOW); digitalWrite(IN2,LOW);
  digitalWrite(IN3,LOW); digitalWrite(IN4,LOW);
  setMotorSpeed(0,0);
}

void driveForward(uint8_t s){
  digitalWrite(IN1,HIGH); digitalWrite(IN2,LOW);
  digitalWrite(IN3,HIGH); digitalWrite(IN4,LOW);
  setMotorSpeed(s,s);
}

void driveBackward(uint8_t s){
  digitalWrite(IN1,LOW); digitalWrite(IN2,HIGH);
  digitalWrite(IN3,LOW); digitalWrite(IN4,HIGH);
  setMotorSpeed(s,s);
}

void driveLeft(uint8_t s){
  digitalWrite(IN1,LOW); digitalWrite(IN2,HIGH);
  digitalWrite(IN3,HIGH); digitalWrite(IN4,LOW);
  setMotorSpeed(s,s);
}

void driveRight(uint8_t s){
  digitalWrite(IN1,HIGH); digitalWrite(IN2,LOW);
  digitalWrite(IN3,LOW); digitalWrite(IN4,HIGH);
  setMotorSpeed(s,s);
}

// ================= Ultrasonic =================
float getDistanceCm(){
  digitalWrite(TRIG_PIN,LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN,HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN,LOW);
  unsigned long d = pulseIn(ECHO_PIN,HIGH,30000);
  if(d==0) return -1;
  return d * 0.0343 / 2;
}

unsigned long lastDbg = 0;
void debugDistance(float d){
  if(millis() - lastDbg > 300){
    lastDbg = millis();
    Serial.print("Distance: ");
    if(d < 0) Serial.println("no echo (-1)");
    else { Serial.print(d); Serial.println(" cm"); }
  }
}

// ================= Manual =================
void handleManual(){
  if((forwardCmd||backCmd||leftCmd||rightCmd) && millis()-lastCmdTime > CMD_TIMEOUT){
    forwardCmd=backCmd=leftCmd=rightCmd=false;
  }

  if(forwardCmd) driveForward(manualSpeed);
  else if(backCmd) driveBackward(manualSpeed);
  else if(leftCmd) driveLeft(manualSpeed);
  else if(rightCmd) driveRight(manualSpeed);
  else driveStop();
}

// ================= Obstacle =================
void handleObstacle(){
  unsigned long now = millis();
  switch(autoState){
    case AUTO_IDLE: autoState = AUTO_FORWARD; break;
    case AUTO_FORWARD:{
      float d = getDistanceCm();
      debugDistance(d);
      if(d > 0 && d < OBSTACLE_DIST_CM){
        driveStop();
        autoState = AUTO_BACKWARD;
        autoStateStart = now;
      } else driveForward(autoSpeed);
    }break;
    case AUTO_BACKWARD:
      if(now - autoStateStart >= BACK_TIME){
        autoState = AUTO_TURN;
        autoStateStart = now;
      } else driveBackward(autoSpeed);
      break;
    case AUTO_TURN:
      turnLeftNext ? driveLeft(autoSpeed) : driveRight(autoSpeed);
      if(now - autoStateStart >= TURN_TIME){
        driveStop();
        turnLeftNext = !turnLeftNext;
        autoState = AUTO_FORWARD;
      }
      break;
  }
}

// ================= Follow =================
void handleFollow(){
  float d = getDistanceCm();
  debugDistance(d);
  if(d < 0){ driveStop(); return; }
  if(d < 12) driveBackward(autoSpeed);
  else if(d <= 20) driveStop();
  else if(d <= 45) driveForward(autoSpeed);
  else driveStop();
}

// ================= HTTP =================
void handleRoot(){ server.send_P(200,"text/html",index_html); }

void handleCmd(){
  if(obstacleMode || followMode){
    server.send(200,"text/plain","IGNORED");
    return;
  }
  String d = server.arg("dir");
  bool p = server.arg("state").toInt() != 0;

  bool* flag = nullptr;
  if(d == "f") flag = &forwardCmd;
  else if(d == "b") flag = &backCmd;
  else if(d == "l") flag = &leftCmd;
  else if(d == "r") flag = &rightCmd;

  if(flag){
    if(*flag != p){
      Serial.print("Commande: "); Serial.print(d);
      Serial.print(" | State: "); Serial.println(p);
    }
    *flag = p;
    lastCmdTime = millis();
  }
  else if(d == "s"){
    forwardCmd = backCmd = leftCmd = rightCmd = false;
    driveStop();
  }
  server.send(200,"text/plain","OK");
}

void handleMode(){
  obstacleMode = server.arg("auto").toInt();
  Serial.print("Obstacle mode: "); Serial.println(obstacleMode);
  followMode = false;
  autoState = AUTO_FORWARD;
  forwardCmd = backCmd = leftCmd = rightCmd = false;
  driveStop();
  server.send(200,"text/plain","OK");
}

void handleFollowHttp(){
  followMode = server.arg("f").toInt();
  Serial.print("Follow mode: "); Serial.println(followMode);
  obstacleMode = false;
  forwardCmd = backCmd = leftCmd = rightCmd = false;
  driveStop();
  server.send(200,"text/plain","OK");
}

// ================= SETUP =================
void setup(){
  Serial.begin(115200);

  pinMode(IN1,OUTPUT); pinMode(IN2,OUTPUT);
  pinMode(IN3,OUTPUT); pinMode(IN4,OUTPUT);

  // تهيئة الـ PWM حسب إصدارات ESP32 الحديثة
  ledcAttach(ENA_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(ENB_PIN, PWM_FREQ, PWM_RES);

  pinMode(TRIG_PIN,OUTPUT);
  pinMode(ECHO_PIN,INPUT);

  driveStop();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid, apPassword);
  Serial.println("Wi-Fi ESP32_Car demarre!");

  server.on("/", handleRoot);
  server.on("/cmd", handleCmd);
  server.on("/mode", handleMode);
  server.on("/follow", handleFollowHttp);
  server.begin();
}

// ================= LOOP =================
void loop(){
  server.handleClient();
  if(followMode) handleFollow();
  else if(obstacleMode) handleObstacle();
  else handleManual();
  delay(5);
}