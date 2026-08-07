# count primes below 50
# expect: 15
c=0
n=2
while n<50:
    p=1
    d=2
    while d*d<=n:
        if n%d==0: p=0
        d=d+1
    c=c+p
    n=n+1
print(c)
