#pragma once

class ArgumentHandler {
    
    private:
        int argc;
        const char** argv;
    public:
        ArgumentHandler(int _argc, char** _argv) : argc(_argc), argv(_argv) {}
        
};
