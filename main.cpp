#include <iostream>
#include <fstream>
#include <vector>
#include <list>
#include <string>
#include <array>
#include <regex>
#include <iomanip>
#include <optional>
#include <filesystem>
using namespace std;
namespace fs = std::filesystem;

enum struct machineState{START_END, OUTSIDE_PERIMETERS, IN_PERIMETERS, EXIT};
enum struct GCodeType{G0,G1,G2,G3,NONE};

struct movement{
  bool overhang = false;
  float lineWidth = 0.0;
  float lineHeight = 0.0;
  array<float,3> endPos = {0,0,0};
  GCodeType type=GCodeType::NONE; //G0 G1 G2 G3
  array<float,2> IJ = {0,0}; //update only for G2,G3
  float acceleration = 0.0; //according to M204 Sxxx
  float fanSpeed = 0.0; //according to M204 Sxxx
  float speed = 0.0; //according to Fxxx
  float extrusionLength = 0.0; //E parameter
};

machineState processStartEnd(ifstream& file, list<string>& outputGCode);
machineState processOutsidePerimeters(ifstream& file, list<string>& outputGCode, movement& lastMove);
machineState processIternalPerimeters(ifstream& file, list<string>& outputGCode, movement& lastMove);
bool updateMovement(const string& line, movement& m);
list<string> reverseMovementList(list<movement>& fwdMoves);
list<string> interpretBwdMovement(movement current, movement previous, movement next);

machineState processStartEnd(ifstream& file, list<string>& outputGCode){
    string currentLine;
    
    while(getline(file,currentLine)){
        outputGCode.push_back(currentLine);
        if(currentLine.find("; printing object") == 0){
            return machineState::OUTSIDE_PERIMETERS;
        }
    }
    return machineState::EXIT;
}

machineState processOutsidePerimeters(ifstream& file, list<string>& outputGCode, movement& lastMove){
    string currentLine;
    
    while(getline(file, currentLine)){
        outputGCode.push_back(currentLine);
        updateMovement(currentLine,lastMove);
        if(currentLine.find("; stop printing object") == 0){
            return machineState::START_END;
        }
        if(currentLine==";TYPE:Perimeter"){
            return machineState::IN_PERIMETERS;
        }
    }
    return machineState::EXIT;
}

machineState processIternalPerimeters(ifstream& file, list<string>& outputGCode, movement& lastMove){
    string currentLine;
    list<movement> perimeterMoves;
    
    perimeterMoves.push_back(lastMove);//add the last known state to the list
    while(getline(file,currentLine)){
        if(updateMovement(currentLine,lastMove)){
            perimeterMoves.push_back(lastMove);
        }
        if(currentLine.find(";TYPE:") == 0 && currentLine.substr(6)!="Perimeter"&&currentLine.substr(6)!="Overhang perimeter"){
            lastMove.type=GCodeType::NONE;
            perimeterMoves.push_back(lastMove);//to ensure all internal states are saved
            outputGCode.splice(outputGCode.end(),reverseMovementList(perimeterMoves));
            outputGCode.push_back(currentLine);
            return machineState::OUTSIDE_PERIMETERS;
        }
        if(currentLine.find("; stop printing object") == 0){
            lastMove.type=GCodeType::NONE;
            perimeterMoves.push_back(lastMove);//to ensure all internal states are saved
            outputGCode.splice(outputGCode.end(),reverseMovementList(perimeterMoves));
            outputGCode.push_back(currentLine);
            return machineState::START_END;
        }
    }
    return machineState::EXIT;
}

bool updateMovement(const string& line, movement& m) { // return true si dX dY dZ E !=0
    if (line.empty()) return false;

    // 1.Metadata Parsing(PrusaSlicer Comments)
    if (line[0] == ';') {
        if (line.find(";TYPE:") == 0) {
            m.overhang = line.substr(6)=="Overhang perimeter";
        }
        if (line.find(";WIDTH:") == 0) m.lineWidth = stof(line.substr(7));
        if (line.find(";HEIGHT:") == 0) m.lineHeight = stof(line.substr(8));
        return false;
    }

    // 2. M commands Parsing
    if (line.find("M204 S") == 0) {//acceleration
        m.acceleration = stof(line.substr(6));
        return false;
    }
    
    if (line.find("M106 S") == 0) {//acceleration
        m.fanSpeed = stof(line.substr(6));
        return false;
    }

    // 3. Parsing G0-G3 movements

    int cmdNum;
    if (sscanf(line.c_str(), "G%d", &cmdNum) == 1) {
        switch(cmdNum){
            case 0:
                m.type = GCodeType::G0;
                break;
            case 1:
                m.type = GCodeType::G1;
                break;
            case 2:
                m.type = GCodeType::G2;
                break;
            case 3:
                m.type = GCodeType::G3;
                break;
            default:
                return false;
        }
        auto getParam = [&line](char p) -> optional<float> {
            size_t pos = line.find(p);
            if (pos != string::npos) {
                try {
                    return stod(line.substr(pos + 1));
                } catch (...) {
                    return nullopt;
                }
            }
            return std::nullopt;
        };
        bool moved=false;
        // Update only if value is available
        if (auto val = getParam('X')){
            m.endPos[0] = *val;
            moved=true;
        }
        if (auto val = getParam('Y')){ 
            m.endPos[1] = *val;
            moved=true;
        }
        if (auto val = getParam('Z')){
            m.endPos[2] = *val;
            moved=true;
        }
        if (auto val = getParam('E')){
            m.extrusionLength = (float)*val;
            moved=true;
        }
        else{
            m.extrusionLength = 0;
        }
        if (auto val = getParam('F')){ m.speed = (float)*val;}
        if (auto val = getParam('I')){ m.IJ[0] = *val;}
        if (auto val = getParam('J')){ m.IJ[1] = *val;}

        return moved; // move or extusion has been captured
    }
    return false;
}

list<string> reverseMovementList(list<movement>& fwdMoves){
    if (fwdMoves.size() < 2) return {};
    
    list<string>bwdMoves;
    movement lastOriginal = fwdMoves.back();
    if(lastOriginal.type==GCodeType::NONE){
        fwdMoves.pop_back();//remove last line if it only contains info about state info
    }
    movement firstOriginal = fwdMoves.front();//last known state before entering perimeters section
        // 1. Mouvement de transition (G0) vers la fin du périmètre (qui devient le début)
    ostringstream oss;
    oss << fixed << setprecision(3) << "G0 X" << lastOriginal.endPos[0] << " Y" << lastOriginal.endPos[1];
    bwdMoves.push_back(oss.str());
        // 2. Inversion
    movement nextMovement = firstOriginal;
    nextMovement.endPos = lastOriginal.endPos;
    
    list<movement>::reverse_iterator it = fwdMoves.rbegin();
    while (it != prev(fwdMoves.rend())) {
        movement currentMovement = *it;
        movement previousMovement = *(next(it));
        
        list<string> lines = interpretBwdMovement(currentMovement, previousMovement, nextMovement);
        bwdMoves.splice(bwdMoves.end(), lines);
        
        nextMovement = currentMovement;
        ++it;
    }
    //3.mouvement de transition vers la position de fin originale forcage de l'accélération et vitesse
    oss.str(string());
    oss << "G1" << " X " << lastOriginal.endPos[0] << " Y " << lastOriginal.endPos[1]<< " Z " << lastOriginal.endPos[2];
    oss << " F " << (int)lastOriginal.speed;
    bwdMoves.push_back(oss.str());
    oss.str(string());
    oss << "M204 S" << (int)lastOriginal.acceleration;
    bwdMoves.push_back(oss.str());
    return bwdMoves;
}

list<string> interpretBwdMovement(movement current, movement previous, movement next){
    list<string>reversedMovement;
    ostringstream textLine;
    textLine << fixed;
    //;TYPE:Perimeter / Overhang Perimeter
    if(current.overhang!=next.overhang){
        textLine<<";TYPE:"<<(current.overhang?"Overhang Perimeter":"Perimeter");
        reversedMovement.push_back(textLine.str());
        textLine.str(string());
    }
    //;WIDTH: 0.000000
    if(current.lineWidth!=next.lineWidth){
        textLine<<";WIDTH:"<<setprecision(6)<<current.lineWidth;
        reversedMovement.push_back(textLine.str());
        textLine.str(string());
    }
    //;Height: 0.000000
    if(current.lineHeight!=next.lineHeight){
        textLine<<";HEIGHT:"<<setprecision(6)<<current.lineHeight;
        reversedMovement.push_back(textLine.str());
        textLine.str(string());
    }
    //M204 S0
    if(current.acceleration!=next.acceleration){
        textLine<<"M204 S"<<(int)current.acceleration;
        reversedMovement.push_back(textLine.str());
        textLine.str(string());
    }
    if(current.fanSpeed!=next.fanSpeed){
        textLine<<"M106 S"<<(int)current.fanSpeed;
        reversedMovement.push_back(textLine.str());
        textLine.str(string());
    }
    //G1 F0
    if(current.speed!=next.speed){
        textLine<<"G1 F"<<(int)current.speed;
        reversedMovement.push_back(textLine.str());
        textLine.str(string());
    }
    
    //G0/G1/G2/G3 X0.000 Y0.000 Z0.000 E0.00000
    switch(current.type){
        case GCodeType::G0:
            textLine<<"G0";
            break;
        case GCodeType::G1:
            textLine<<"G1";
            break;
        case GCodeType::G2://clockwise becomes counterclockwise when reversing
            textLine<<"G3";
            break;
        case GCodeType::G3://counterclockwise becomes clockwise when reversing
            textLine<<"G2";
            break;
        default:
            return reversedMovement;
    }
    
    if(current.endPos[0]!=previous.endPos[0]||current.endPos[1]!=previous.endPos[1]){
        textLine<<" X"<<setprecision(3)<<previous.endPos[0];
        textLine<<" Y"<<setprecision(3)<<previous.endPos[1];
    }
    if(current.endPos[2]!=previous.endPos[2]){
        textLine<<" Z"<<setprecision(3)<<previous.endPos[2];
    }
    
    if(current.type==GCodeType::G2||current.type==GCodeType::G3){
        float xc = previous.endPos[0]+current.IJ[0];
        float yc = previous.endPos[1]+current.IJ[1];
        float newI = xc-current.endPos[0];
        float newJ = yc-current.endPos[1];
        textLine<<" I"<<newI<<" J"<<newJ;
    }
    
    if(current.extrusionLength!=0){
        textLine<<" E"<<setprecision(5)<<current.extrusionLength;
    }
    reversedMovement.push_back(textLine.str());
    return reversedMovement;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <input_file.gcode>" << endl;
        return 1;
    }

    string inputPath = argv[1];
    string tempPath = inputPath + ".tmp";

    ifstream file(inputPath);
    if (!file.is_open()) {
        cerr << "Error : unable to open source file." << endl;
        return 1;
    }

    list<string> output;
    movement m;
    machineState state = machineState::START_END;

    // Pocessing
    while (state != machineState::EXIT) {
        switch (state) {
            case machineState::START_END: state = processStartEnd(file, output); break;
            case machineState::OUTSIDE_PERIMETERS: state = processOutsidePerimeters(file, output, m); break;
            case machineState::IN_PERIMETERS: state = processIternalPerimeters(file, output, m); break;
            default: state = machineState::EXIT; break;
        }
    }
    file.close();

    // Writing to the temporary file
    ofstream outFile(tempPath);
    if (!outFile.is_open()) {
        cerr << "Error : impossible to create tempoary file." << endl;
        return 1;
    }

    for (const auto& s : output) {
        outFile << s << "\n";
    }
    outFile.close();

    // Remplacement of original file by the temporary one
    try {
        fs::rename(tempPath, inputPath);
        cout << "File successfully processed and overwritten : " << inputPath << endl;
    } catch (const fs::filesystem_error& e) {
        cerr << "Error while replacing file : " << e.what() << endl;
        return 1;
    }

    return 0;
}
