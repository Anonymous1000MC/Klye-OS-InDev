import sys
f=open(sys.argv[1],'rb'); f.readline(); w,h=map(int,f.readline().split()); f.readline(); d=f.read()
def px(x,y):
    o=(y*w+x)*3; return d[o],d[o+1],d[o+2]
sh=' .:-=+*#%@'
step=int(sys.argv[2]) if len(sys.argv)>2 else 10
for y in range(0,h,step*2):
    row=''
    for x in range(0,w,step):
        c=px(x,y); lum=(c[0]*3+c[1]*6+c[2])//10
        row+=sh[min(9,lum*9//255)]
    print(row)
