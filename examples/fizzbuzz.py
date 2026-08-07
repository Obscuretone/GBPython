# fizzbuzz 1..15 (output window shows the last 5 lines)
# expect: 11
# expect: fizz
# expect: 13
# expect: 14
# expect: fizzbuzz
for i in range(1,16):
    if i%15==0: print('fizzbuzz')
    elif i%3==0: print('fizz')
    elif i%5==0: print('buzz')
    else: print(i)
