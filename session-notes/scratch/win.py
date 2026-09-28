import sys
path=sys.argv[1]; x0,y0,x1,y1,sx,sy=map(int,sys.argv[2:8])
f=open(path,'rb'); f.readline(); w,h=map(int,f.readline().split()); f.readline(); d=f.read()
def px(x,y):
    o=(y*w+x)*3; return (d[o],d[o+1],d[o+2])
sh=' .:-=+*#%@'
for y in range(y0,min(y1,h),sy):
    row=''
    for x in range(x0,min(x1,w),sx):
        c=px(x,y)
        row+=sh[min(9,((c[0]*3+c[1]*6+c[2])//10)*9//255)]
    print(row)
