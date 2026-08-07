# bubble sort a list
# expect: [1, 2, 3, 5, 9]
a=[5,3,9,1,2]
n=len(a)
for i in range(n):
    for j in range(n-1-i):
        if a[j]>a[j+1]:
            t=a[j]
            a[j]=a[j+1]
            a[j+1]=t
print(a)
