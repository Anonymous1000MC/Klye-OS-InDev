import sys
path,x0,y0,x1,y1,sx,sy = sys.argv[1],*map(int,sys.argv[2:8])
f=open(path,'rb'); f.readline(); w,h=map(int,f.readline().split()); f.readline(); d=f.read()
def px(x,y):
    o=(y*w+x)*3; return d[o],d[o+1],d[o+2]
sh=' .:-=+*#%@'
for y in range(y0,min(y1,h),sy):
    row=''
    for x in range(x0,min(x1,w),sx):
        c=px(x,y); lum=(c[0]*3+c[1]*6+c[2])//10
        row+=sh[min(9,lum*9//255)]
    print(row)
