//
// Created by lidor on 5/14/2021.
//

#include <algorithm>
#include <cctype>
#include <string>

#include "Types.h"
#include "registerAllocator.h"
using namespace std;
extern RegisterAllocator regAllocator;

bool isNumeric(const string &type) {
    if (type == TYPE_BYTE || type == TYPE_INT)
        return true;
    return false;
}

void checkForValidType(const string &type) {
    static const string valid_types[] = {
            TYPE_VOID,
            TYPE_BOOL,
            TYPE_INT,
            TYPE_STRING,
            TYPE_BYTE,
    };

    for (unsigned int i=0; i < 5; i++) {
        if (type == valid_types[i])
            return;
    }
    cout << "found invalid type: " << type << endl;
    errorMismatch(yylineno);
    exit(-1);
}

bool assertAssignableTypes(const string &leftType, const string &leftId, const string &rightType, const string &rightId, bool toExit, bool printError) {
    string r_type = (rightType == "unknown_type" || rightType == TYPE_ID) ? symbolTable.getReturnType(rightId) : rightType;
    string l_type = (leftType  == "unknown_type"  || leftType == TYPE_ID) ? symbolTable.getReturnType(leftId) : leftType;
    //cout << "Left: " << leftId << " " << l_type << " Right: " << rightId << " " << r_type << endl;
    if (r_type != l_type) {
        if (!((l_type == TYPE_INT) && (r_type == TYPE_BYTE))) {
            if (printError)
                errorMismatch(yylineno);
            if (toExit)
                exit(-1);
            return false;
        }
    }
    return true;
}

void assertBoolean(Node *exp) {
    //cout << exp->id << exp->type << exp->realtype() << endl;
    string type = exp->realtype();
    if (type == TYPE_ID) {
        type = symbolTable.getReturnType(exp->id);
    }
    if (type != TYPE_BOOL) {
        // cout << "assertBoolean: found type of " << type << " instead of bool" << endl;
        errorMismatch(yylineno);
        exit(-1);
    }
}

void errorByteTooLargeAndExit(int lineno, int64_t value) {
    stringstream ss;
    string s;
    ss << value;
    ss >> s;
    errorByteTooLarge(lineno, s);
    exit(-1);
}

string Node::realtype() {
    string type = this->type;
    if (type == TYPE_ID) {
        // cout << "getting realtype of " << this->id << " - " << yylineno << endl;
        type = symbolTable.getReturnType(this->id);
    }
    return type;
}

void Node::loadExp() {
    if (this->type == TYPE_ID) {
        this->value = regAllocator.loadVar(this->id);
    }
}

string getLlvmType(string type) {
    string llvmType = type;
    if (llvmType == TYPE_VOID) {
        llvmType = "void";
    } else {
        llvmType = "i32";
    }
    return llvmType;
}

void createLlvmArguments(int numArguments, stringstream &code, vector<Node *>* expressions) {
    code << "(";
    for (int i = 0; i < numArguments; i++) {
        if (i != 0) {
            code << ", ";
        }
        if (expressions && (*expressions)[i]->type == TYPE_STRING) {
            // cout << (*expressions)[i]->id << " register is: " << regAllocator.getVarRegister((*expressions)[i]->id);
            code << "i8*";
        } else {
            code << "i32";
        }
        if (expressions) {
            string id = (*expressions)[i]->id;
            string value = regAllocator.getVarRegister(id, (*expressions)[i]->value);

            code << " " << value;
        }
    }
    code << ")";
}

void Node::addBreak() {
    int jmpInstr = buffer.emit("br label @");
    this->nextList.push_back({jmpInstr, FIRST});
}

void Node::addContinue() {
    int jmpInstr = buffer.emit("br label @");
    this->startLoopList.push_back({jmpInstr, FIRST});
}

void Node::emitWhileOpen() {
    this->nextInstruction = buffer.genLabelNextLine();
}

void Node::emitSwitchOpen() {
    int jmpInstr = buffer.emit("br label @");
    this->nextList.push_back({jmpInstr, FIRST});
}

void Node::emitWhileEnd(string whileStartLabel, Node *whileExp) {
    // backpatch and jump to the start of the loop
    //cout << "emitWhileEnd" << endl;
    buffer.bpatch(this->startLoopList, whileStartLabel);
    stringstream code;
    code << "br label %" << whileStartLabel;
    buffer.emit(code.str());
    // label the loop exit
    string whileEndLabel = buffer.genLabel();
    buffer.bpatch(this->nextList, whileEndLabel);
    buffer.bpatch(whileExp->falseList, whileEndLabel);
    buffer.bpatch(whileExp->trueList, whileStartLabel);
}

void Node::emitWhileExp(string cmpReg) {
    //cout << "emitWhileExp: start" << endl;
    stringstream code;
    string whileExpReg = regAllocator.getNextRegisterName();
    code << whileExpReg << " = " << "trunc i32 " << cmpReg << " to i1" << endl;
    code << "br i1 " << whileExpReg << ", label @, label @";
    int cmpAddress = buffer.emit(code.str());
    string whileStartCodeLabel = buffer.genLabel();
    vector<pair<int,BranchLabelIndex>> v1 = {{cmpAddress, FIRST}};
    buffer.bpatch(v1, whileStartCodeLabel);
    this->falseList.push_back({cmpAddress, SECOND});
    //cout << "emitWhileExp: finish" << endl;
}

void Node::emitCaseLabel() {
    /*
     * fix - 'expected instruction opcode' error when there is no 'break' in a case.
     * https://zanopia.wordpress.com/2010/09/14/understanding-llvm-assembly-with-fractals-part-i/
     * so right before the case label we add an unconditional jmp to that label.
     */
    this->nextInstruction = buffer.genLabelNextLine();
}

void Node::mergeLists(Node *node_a, Node *node_b) {
    // cout << "merging lists" << endl;
    this->nextList = buffer.merge(node_a->nextList, node_b->nextList);
    //this->trueList = buffer.merge(node_a->trueList, node_b->trueList);
    //this->falseList = buffer.merge(node_a->falseList, node_b->falseList);
    this->startLoopList = buffer.merge(node_a->startLoopList, node_b->startLoopList);
}

void Node::emitCallCode(Node* node) {
    //cout << "start emitCallCode" << endl;
    ExpList* expListNode = nullptr;
    vector<Node *> *expressions = nullptr;
    int size = 0;
    if (node) {
        expListNode = (ExpList*)node;
        size = expListNode->size();
        expressions = expListNode->getExpressions();
    }
    stringstream code;
    if (this->id == "print") {
        StringExp* s = (StringExp*)((*expressions)[0]);
        code << s->value << " = getelementptr " << s->str_length << ", " << s->str_length << "* " << s->var << ", i32 0, i32 0" << endl;
    }
    string retType = symbolTable.getReturnType(this->id);
    string llvmType = getLlvmType(retType);
    // save the function call result if needed
    if (retType != TYPE_VOID) {
        string reg = regAllocator.getNextRegisterName();
        code << reg << " = ";
        this->value = reg;
    }
    code << "call " << llvmType << " @" << this->id;
    createLlvmArguments(size, code, expressions);
    buffer.emit(code.str());
    //cout << "finish emitCallCode - " << code.str() << endl;
}

void Node::emitReturnCode() {
    stringstream code;
    string reg;
    code << "ret i32 ";
    reg = regAllocator.getVarRegister(this->id, this->value);
    code << reg;
    buffer.emit(code.str());
}

void Node::emitIfCode() {
    // check if the bool is true or false
    string ifReg = regAllocator.getNextRegisterName();
    string code = ifReg + " = icmp ne i32 0, " + this->value;
    buffer.emit(code);
    // emit the branch instruction
    code = "br i1 " + ifReg + ", label @, label @";
    int ifInstr = buffer.emit(code);
    this->trueList.push_back({ifInstr, FIRST});
    this->falseList.push_back({ifInstr, SECOND});
}

void Node::bpatchIf(string falseLabel) {
    buffer.bpatch(this->trueList, this->nextInstruction);
    this->trueList.clear();
    buffer.bpatch(this->falseList, falseLabel);
    this->falseList.clear();
}

void Node::emitElseCode() {
    // jmp from the end of the if
    int jmpInstr = buffer.emit("br label @");
    // mark the start of the else code
    this->nextInstruction = buffer.genLabelNextLine();
    this->nextList.push_back({jmpInstr, FIRST});
}

BinaryLogicOp::BinaryLogicOp(Node *left, Node *right, bool isAnd, Node *marker) {
    if (left->realtype() == TYPE_BOOL && right->realtype() == TYPE_BOOL) {
        this->type = TYPE_BOOL;
        this->is_numeric = false;
    } else {
        errorMismatch(yylineno);
        exit(-1);
    }
    if (isAnd) {
        //buffer.emit("AND");
        int rightJmpInstr = buffer.emit("br label @");
        string rightLabel = buffer.genLabel();
        buffer.bpatch(marker->nextList, rightLabel);
        buffer.bpatch(left->falseList, rightLabel);
        this->trueList = right->trueList;
        this->falseList = buffer.merge(left->falseList, right->falseList);
        // check for short circuit evaluation
        string compRegI1 = regAllocator.getNextRegisterName();
        stringstream code;
        left->loadExp();
        code << compRegI1 << " = icmp eq i32 " << regAllocator.getVarRegister(left->id, left->value) << ", 0";
        buffer.emit(code.str());
        stringstream code2;
        code2 << "br i1 " << compRegI1 << ", label @, label @";
        int brInstr = buffer.emit(code2.str());

        // the false label

        string label = buffer.genLabelNextLine();
        compRegI1 = regAllocator.getNextRegisterName();
        string compReg = regAllocator.getNextRegisterName();
        stringstream code3;
        right->loadExp();
        code3 << compRegI1 << " = icmp ne i32 " << regAllocator.getVarRegister(right->id, right->value) << ", 0" << endl;
        code3 << compReg << " = zext i1 " << compRegI1 << " to i32";
        buffer.emit(code3.str());
        int jmpInstr = buffer.emit("br label @");

        // end of short circuit evaluation, create the phi command and backpatch all
        string label2 = buffer.genLabel();
        stringstream code4;
        string resReg = regAllocator.getNextRegisterName();
        code4 << resReg << " = phi i32 [0, @], [" << compReg << ", @]";
        int phiInstr = buffer.emit(code4.str());
        vector<pair<int,BranchLabelIndex>> v1 = {{brInstr, FIRST}, {jmpInstr, FIRST}};
        buffer.bpatch(v1, label2);
        vector<pair<int,BranchLabelIndex>> v2 = {{phiInstr, SECOND}, {rightJmpInstr, FIRST}};
        buffer.bpatch(v2, label);
        vector<pair<int,BranchLabelIndex>> v3 = {{phiInstr, FIRST}};
        buffer.bpatch(v3, rightLabel);
        vector<pair<int,BranchLabelIndex>> v4 = {{brInstr, SECOND}};
        buffer.bpatch(v4, marker->nextInstruction);
        // update the result value to be the last register
        this->value = resReg;
    } else { // or op
        //buffer.emit("OR");
        int rightJmpInstr = buffer.emit("br label @");
        string rightLabel = buffer.genLabel();
        buffer.bpatch(marker->nextList, rightLabel);
        buffer.bpatch(left->falseList, rightLabel);
        this->trueList = buffer.merge(left->trueList, right->trueList);
        this->falseList = right->falseList;
        // check for short circuit evaluation
        string compRegI1 = regAllocator.getNextRegisterName();
        stringstream code;
        left->loadExp();
        code << compRegI1 << " = icmp ne i32 " << regAllocator.getVarRegister(left->id, left->value) << ", 0";
        buffer.emit(code.str());
        stringstream code2;
        code2 << "br i1 " << compRegI1 << ", label @, label @";
        int brInstr = buffer.emit(code2.str());

        // the false label

        string label = buffer.genLabelNextLine();
        compRegI1 = regAllocator.getNextRegisterName();
        string compReg = regAllocator.getNextRegisterName();
        stringstream code3;
        right->loadExp();
        code3 << compRegI1 << " = icmp ne i32 " << regAllocator.getVarRegister(right->id, right->value) << ", 0" << endl;
        code3 << compReg << " = zext i1 " << compRegI1 << " to i32";
        buffer.emit(code3.str());
        int jmpInstr = buffer.emit("br label @");

        // end of short circuit evaluation, create the phi command and backpatch all
        string label2 = buffer.genLabel();
        stringstream code4;
        string resReg = regAllocator.getNextRegisterName();
        code4 << resReg << " = phi i32 [1, @], [" << compReg << ", @]";
        int phiInstr = buffer.emit(code4.str());
        vector<pair<int,BranchLabelIndex>> v1 = {{brInstr, FIRST}, {jmpInstr, FIRST}};
        buffer.bpatch(v1, label2);
        vector<pair<int,BranchLabelIndex>> v2 = {{phiInstr, SECOND}, {rightJmpInstr, FIRST}};
        buffer.bpatch(v2, label);
        vector<pair<int,BranchLabelIndex>> v3 = {{phiInstr, FIRST}};
        buffer.bpatch(v3, rightLabel);
        vector<pair<int,BranchLabelIndex>> v4 = {{brInstr, SECOND}};
        buffer.bpatch(v4, marker->nextInstruction);
        // update the result value to be the last register
        this->value = resReg;
    }
    delete left;
    delete right;
}

RelOp::RelOp(Node *left, Node *right, string op) {
    // cout<< left->id <<" "<< left->type <<" "<< left->realtype() <<" "<< right->id <<" "<< right->type <<" "<< right->realtype()<<endl;
    // cout << "op = " << op << endl;
    if (isNumeric(left->realtype()) && isNumeric(right->realtype())) {
        this->type = TYPE_BOOL;
        this->is_numeric = false;
    } else {
        errorMismatch(yylineno);
        exit(-1);
    }
    left->loadExp();
    right->loadExp();
    this->value = regAllocator.emitCmpCode(left->value, right->value, op);
    delete left;
    delete right;
}

void CaseList::emitCase(vector<pair<int, BranchLabelIndex>> &nextList, string switchVal) {
    // first create jmp after default and backpatch the initial jump
    int defaultJmp = buffer.emit("br label @");
    vector<pair<int, BranchLabelIndex>> v1 = {{defaultJmp, FIRST}};
    buffer.bpatch(nextList, buffer.genLabel());
    // now emit the switch with the llvm syntax
    string code = "switch i32 " + switchVal + ", label @ [ ";
    for(auto it = this->caseList.begin(); it != this->caseList.end(); ++it) {
        code += "i32 " + (*it).first + ", label %" + (*it).second + " ";
    }
    code += "]";
    int switchInstr = buffer.emit(code);
    string afterSwitchLabel = buffer.genLabel();
    vector<pair<int, BranchLabelIndex>> v2 = {{switchInstr, FIRST}};
    if (this->hasDefault) {
        buffer.bpatch(v2, this->defaultLabel);
    } else {
        buffer.bpatch(v2, afterSwitchLabel);
    }
    // backpatch all the break in the switch
    buffer.bpatch(this->nextList, afterSwitchLabel);
    buffer.bpatch(v1, afterSwitchLabel);
}

StringExp::StringExp(string _str) {
    this->type = TYPE_STRING;
    this->id = _str;
    string string_var = regAllocator.createStringConstant();
    this->value = "%" + string_var;
    this->var = "@." + string_var;
    this-> str_length = "[" + to_string(_str.length() - 1) + " x i8]";
    buffer.emitGlobal(this->var + " = constant " + this->str_length + "c" + _str.replace(_str.length() - 1, 1, "\\00\""));
}

UnaryLogicOp::UnaryLogicOp(Node *exp) {
    exp->loadExp();
    string type = exp->realtype();
        if (type == TYPE_BOOL) {
        this->type = TYPE_BOOL;
        this->is_numeric = false;
    } else {
        //cout << "UnaryLogicOp: found type of " << type << endl;
        errorMismatch(yylineno);
        exit(-1);
    }
    this->value = regAllocator.getNextRegisterName();
    buffer.emit(this->value + " = sub i32 1, " + exp->value);
    this->trueList = exp->falseList;
    this->falseList = exp->trueList;
    delete exp;
}
