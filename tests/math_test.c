#include <stdio.h>
#include <math.h>
extern double sqrt(double),sin(double),cos(double),tan(double),exp(double),log(double);
extern double pow(double,double),fmod(double,double),atan2(double,double),floor(double);
extern double ceil(double),log2(double),fabs(double),asin(double),acos(double),atan(double);
static int fails=0;
static void near(const char*n,double got,double want,double tol){
    double d=got-want; if(d<0)d=-d;
    if(d>tol){printf("FAIL %-8s got %.12g want %.12g\n",n,got,want);fails++;}
    else printf("ok   %-8s %.10g\n",n,got);
}
int main(void){
    near("sqrt2",sqrt(2.0),1.4142135623730951,1e-12);
    near("sqrt1e8",sqrt(1e8),10000.0,1e-9);
    near("sin(1)",sin(1.0),0.8414709848078965,1e-12);
    near("sin(pi)",sin(3.141592653589793),0.0,1e-12);
    near("cos(0)",cos(0.0),1.0,1e-15);
    near("cos(1)",cos(1.0),0.5403023058681398,1e-12);
    near("sin(100)",sin(100.0),-0.5063656411097588,1e-11);
    near("tan(1)",tan(1.0),1.5574077246549023,1e-10);
    near("exp(1)",exp(1.0),2.718281828459045,1e-12);
    near("exp(10)",exp(10.0),22026.465794806718,1e-8);
    near("exp(-5)",exp(-5.0),0.006737946999085467,1e-15);
    near("log(e)",log(2.718281828459045),1.0,1e-12);
    near("log(1000)",log(1000.0),6.907755278982137,1e-12);
    near("log2(1024)",log2(1024.0),10.0,1e-12);
    near("pow(2,10)",pow(2.0,10.0),1024.0,0.0);
    near("pow(2,0.5)",pow(2.0,0.5),1.4142135623730951,1e-12);
    near("pow(10,3)",pow(10.0,3.0),1000.0,0.0);
    near("fmod",fmod(7.5,2.0),1.5,1e-15);
    near("fmod neg",fmod(-7.5,2.0),-1.5,1e-15);
    near("atan2(1,1)",atan2(1.0,1.0),0.7853981633974483,1e-12);
    near("atan2(0,-1)",atan2(0.0,-1.0),3.141592653589793,1e-12);
    near("asin(0.5)",asin(0.5),0.5235987755982988,1e-12);
    near("acos(0.5)",acos(0.5),1.0471975511965976,1e-12);
    near("atan(1)",atan(1.0),0.7853981633974483,1e-12);
    near("floor(-2.5)",floor(-2.5),-3.0,0.0);
    near("ceil(-2.5)",ceil(-2.5),-2.0,0.0);
    near("floor(2.5)",floor(2.5),2.0,0.0);
    printf(fails?"MATH FAILURES: %d\n":"all math checks passed\n",fails);
    return 0;
}
