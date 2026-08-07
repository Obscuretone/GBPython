# tenth fibonacci number
# expect: 55
a=0
b=1
for i in range(10):
    t=a+b
    a=b
    b=t
print(a)
